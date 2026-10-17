#include "provider_json.h"

#include <cstdlib>
#include <cstring>

namespace football {
namespace {
struct alignas(std::max_align_t) Header {
  std::size_t size;
};
}  // namespace
void* JsonAllocator::allocate(std::size_t size) {
  if (size > cfg::kJsonHeapBytes - used_ ||
      sizeof(Header) > cfg::kJsonHeapBytes - used_ - size)
    return nullptr;
  auto* p = static_cast<Header*>(std::malloc(sizeof(Header) + size));
  if (p == nullptr) return nullptr;
  p->size = size;
  used_ += size + sizeof(Header);
  return p + 1;
}
void JsonAllocator::deallocate(void* pointer) {
  if (pointer == nullptr) return;
  auto* p = static_cast<Header*>(pointer) - 1;
  used_ -= p->size + sizeof(Header);
  std::free(p);
}
void* JsonAllocator::reallocate(void* pointer, std::size_t size) {
  if (pointer == nullptr) return allocate(size);
  auto* p = static_cast<Header*>(pointer) - 1;
  const auto old = p->size;
  if (size > cfg::kJsonHeapBytes - (used_ - old)) return nullptr;
  auto* next = static_cast<Header*>(std::realloc(p, sizeof(Header) + size));
  if (next == nullptr) return nullptr;
  next->size = size;
  used_ = used_ - old + size;
  return next + 1;
}
const char* jsonElementFields(cfg::Provider provider, const char* array_key) {
  if (provider != cfg::Provider::kEspn || array_key == nullptr) return nullptr;
  if (strcmp(array_key, "events") == 0)
    return R"({"id":true,"date":true,"season":{"year":true},"competitions":[{"timeValid":true,)"
           R"("status":{"type":{"state":true,"name":true,"completed":true},"displayClock":true},)"
           R"("competitors":[{"homeAway":true,"team":{"id":true,"displayName":true,"shortDisplayName":true},"score":true,"shootoutScore":true}],)"
           R"("details":[{"type":{"text":true},"shootout":true,"ownGoal":true,"penaltyKick":true,"scoringPlay":true,)"
           R"("athletesInvolved":[{"displayName":true}],"clock":{"displayValue":true},"team":{"id":true}}]}]})";
  if (strcmp(array_key, "entries") == 0)
    return R"({"team":{"id":true,"displayName":true,"shortDisplayName":true},"stats":[{"name":true,"value":true}]})";
  if (strcmp(array_key, "keyEvents") == 0)
    return R"({"id":true,"type":{"type":true},"participants":[{"athlete":{"displayName":true}}],"clock":{"displayValue":true},"team":{"id":true}})";
  return nullptr;
}
bool jsonValueHasKind(ArduinoJson::JsonVariantConst value, JsonValueKind kind) {
  switch (kind) {
    case JsonValueKind::kAny: return true;
    case JsonValueKind::kObject:
      return value.is<ArduinoJson::JsonObjectConst>();
    case JsonValueKind::kArray: return value.is<ArduinoJson::JsonArrayConst>();
    case JsonValueKind::kString: return value.is<const char*>();
    case JsonValueKind::kBoolean: return value.is<bool>();
    case JsonValueKind::kNumber: return !value.is<bool>() && value.is<double>();
  }
  return false;
}
bool validJsonObserver(const JsonObserver* observer, const char* array_key) {
  if (observer == nullptr || observer->count == 0) return true;
  // A whole-body or root-array primary consumes that subtree itself.
  if (observer->count > kJsonObserverFields || observer->fields == nullptr ||
      observer->consume == nullptr || array_key == nullptr || *array_key == 0)
    return false;
  for (uint8_t i = 0; i < observer->count; ++i) {
    const auto& field = observer->fields[i];
    if (field.path == nullptr || field.path_size == 0 ||
        field.path_size > cfg::kJsonDepth ||
        (field.path_size < 16 && field.array_parents >> field.path_size) != 0)
      return false;
    for (uint8_t p = 0; p < field.path_size; ++p) {
      const char* key = field.path[p];
      if (key == nullptr || *key == 0 ||
          std::strlen(key) > kJsonWrapperKeyBytes ||
          std::strcmp(key, array_key) == 0)
        return false;
    }
    for (uint8_t j = 0; j < i; ++j) {
      const auto& other = observer->fields[j];
      const auto common =
          field.path_size < other.path_size ? field.path_size : other.path_size;
      uint8_t p = 0;
      while (p < common && std::strcmp(field.path[p], other.path[p]) == 0) ++p;
      if (p == common) {
        const bool field_parent = field.path_size < other.path_size;
        const bool other_parent = other.path_size < field.path_size;
        const auto count = [](JsonDelivery delivery) {
          return delivery == JsonDelivery::kArrayCount ||
                 delivery == JsonDelivery::kObjectCount;
        };
        if (!((field_parent && count(field.delivery)) ||
              (other_parent && count(other.delivery))))
          return false;
      }
    }
  }
  return true;
}
namespace json_detail {
namespace {
const char* literalText(uint8_t literal) {
  return literal == 0 ? "true" : literal == 1 ? "false" : "null";
}
bool whitespace(int c) {
  return c == ' ' || c == '\r' || c == '\n' || c == '\t';
}
int hexDigit(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace
bool Syntax::expectsValue() const {
  if (depth_ == 0) return !root_done_;
  const auto frame = frames_[depth_ - 1];
  return frame == Frame::kObjectValue || frame == Frame::kArrayFirst ||
         frame == Frame::kArrayValue;
}
void Syntax::finishValue() {
  if (depth_ == 0) {
    if (root_done_) valid_ = false;
    root_done_ = true;
  } else if (frames_[depth_ - 1] == Frame::kObjectValue) {
    frames_[depth_ - 1] = Frame::kObjectAfter;
  } else if (frames_[depth_ - 1] == Frame::kArrayFirst ||
             frames_[depth_ - 1] == Frame::kArrayValue) {
    frames_[depth_ - 1] = Frame::kArrayAfter;
  } else {
    valid_ = false;
  }
}
void Syntax::outside(int c) {
  if (whitespace(c)) return;
  const auto frame = depth_ ? frames_[depth_ - 1] : Frame::kArrayValue;
  if (c == '"') {
    key_ =
        depth_ && (frame == Frame::kObjectFirst || frame == Frame::kObjectKey);
    if (!key_ && !expectsValue()) {
      valid_ = false;
      return;
    }
    mode_ = Mode::kString;
    return;
  }
  if (c == '{' || c == '[') {
    if (!expectsValue() || depth_ == cfg::kJsonDepth + 1) {
      valid_ = false;
      return;
    }
    frames_[depth_++] = c == '{' ? Frame::kObjectFirst : Frame::kArrayFirst;
    return;
  }
  if (c == '}' || c == ']') {
    if (!depth_ ||
        (c == '}' && frame != Frame::kObjectFirst &&
         frame != Frame::kObjectAfter) ||
        (c == ']' && frame != Frame::kArrayFirst &&
         frame != Frame::kArrayAfter)) {
      valid_ = false;
      return;
    }
    --depth_;
    finishValue();
    return;
  }
  if (c == ':') {
    if (!depth_ || frame != Frame::kObjectColon)
      valid_ = false;
    else
      frames_[depth_ - 1] = Frame::kObjectValue;
    return;
  }
  if (c == ',') {
    if (depth_ && frame == Frame::kObjectAfter)
      frames_[depth_ - 1] = Frame::kObjectKey;
    else if (depth_ && frame == Frame::kArrayAfter)
      frames_[depth_ - 1] = Frame::kArrayValue;
    else
      valid_ = false;
    return;
  }
  if (!expectsValue()) {
    valid_ = false;
    return;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    mode_ = Mode::kNumber;
    number_ = c == '-'   ? Number::kSign
              : c == '0' ? Number::kZero
                         : Number::kInteger;
  } else if (c == 't' || c == 'f' || c == 'n') {
    mode_ = Mode::kLiteral;
    literal_ = c == 't' ? 0 : c == 'f' ? 1 : 2;
    literal_position_ = 1;
  } else
    valid_ = false;
}
void Syntax::string(int c) {
  if (utf8_left_) {
    if (c < utf8_low_ || c > utf8_high_) {
      valid_ = false;
      return;
    }
    --utf8_left_;
    utf8_low_ = 0x80;
    utf8_high_ = 0xbf;
    return;
  }
  if (escape_ == 2) {
    const int digit = hexDigit(c);
    if (digit < 0) {
      valid_ = false;
      return;
    }
    unicode_ = static_cast<uint16_t>((unicode_ << 4) | digit);
    if (++unicode_digits_ != 4) return;
    if (high_surrogate_) {
      if (unicode_ < 0xdc00 || unicode_ > 0xdfff) {
        valid_ = false;
        return;
      }
      high_surrogate_ = false;
      escape_ = 0;
    } else if (unicode_ >= 0xd800 && unicode_ <= 0xdbff) {
      high_surrogate_ = true;
      escape_ = 3;
    } else if (unicode_ >= 0xdc00 && unicode_ <= 0xdfff) {
      valid_ = false;
    } else
      escape_ = 0;
    return;
  }
  if (escape_ == 3) {
    if (c != '\\')
      valid_ = false;
    else
      escape_ = 4;
    return;
  }
  if (escape_ == 4) {
    if (c != 'u')
      valid_ = false;
    else {
      escape_ = 2;
      unicode_ = 0;
      unicode_digits_ = 0;
    }
    return;
  }
  if (escape_ == 1) {
    escape_ = 0;
    if (c == 'u') {
      escape_ = 2;
      unicode_ = 0;
      unicode_digits_ = 0;
    } else if (c != '"' && c != '\\' && c != '/' && c != 'b' && c != 'f' &&
               c != 'n' && c != 'r' && c != 't')
      valid_ = false;
    return;
  }
  if (c == '\\') {
    escape_ = 1;
    return;
  }
  if (c == '"') {
    mode_ = Mode::kOutside;
    if (key_)
      frames_[depth_ - 1] = Frame::kObjectColon;
    else
      finishValue();
    return;
  }
  if (c < 0x20) {
    valid_ = false;
    return;
  }
  if (c < 0x80) return;
  utf8_low_ = 0x80;
  utf8_high_ = 0xbf;
  if (c >= 0xc2 && c <= 0xdf)
    utf8_left_ = 1;
  else if (c >= 0xe0 && c <= 0xef) {
    utf8_left_ = 2;
    if (c == 0xe0) utf8_low_ = 0xa0;
    if (c == 0xed) utf8_high_ = 0x9f;
  } else if (c >= 0xf0 && c <= 0xf4) {
    utf8_left_ = 3;
    if (c == 0xf0) utf8_low_ = 0x90;
    if (c == 0xf4) utf8_high_ = 0x8f;
  } else
    valid_ = false;
}
bool Syntax::finishNumber() {
  if (number_ != Number::kZero && number_ != Number::kInteger &&
      number_ != Number::kFraction && number_ != Number::kExponentDigits) {
    valid_ = false;
    return false;
  }
  mode_ = Mode::kOutside;
  finishValue();
  return valid_;
}
void Syntax::number(int c) {
  if (c >= '0' && c <= '9') {
    if (number_ == Number::kZero)
      valid_ = false;
    else if (number_ == Number::kSign)
      number_ = c == '0' ? Number::kZero : Number::kInteger;
    else if (number_ == Number::kDot)
      number_ = Number::kFraction;
    else if (number_ == Number::kExponent || number_ == Number::kExponentSign)
      number_ = Number::kExponentDigits;
    return;
  }
  if (c == '.' && (number_ == Number::kZero || number_ == Number::kInteger)) {
    number_ = Number::kDot;
    return;
  }
  if ((c == 'e' || c == 'E') &&
      (number_ == Number::kZero || number_ == Number::kInteger ||
       number_ == Number::kFraction)) {
    number_ = Number::kExponent;
    return;
  }
  if ((c == '+' || c == '-') && number_ == Number::kExponent) {
    number_ = Number::kExponentSign;
    return;
  }
  if (finishNumber()) outside(c);
}
void Syntax::accept(int c) {
  if (!valid_) return;
  switch (mode_) {
    case Mode::kOutside: outside(c); break;
    case Mode::kString: string(c); break;
    case Mode::kNumber: number(c); break;
    case Mode::kLiteral: {
      const char* word = literalText(literal_);
      if (word[literal_position_] == 0) {
        mode_ = Mode::kOutside;
        finishValue();
        outside(c);
      } else if (word[literal_position_] == c)
        ++literal_position_;
      else
        valid_ = false;
      break;
    }
  }
}
bool Syntax::complete() {
  if (mode_ == Mode::kNumber)
    finishNumber();
  else if (mode_ == Mode::kLiteral &&
           literalText(literal_)[literal_position_] == 0) {
    mode_ = Mode::kOutside;
    finishValue();
  }
  return valid_ && mode_ == Mode::kOutside && depth_ == 0 && root_done_;
}
}  // namespace json_detail
}  // namespace football
