#include "json_stream.h"

#include <cstring>
#include <limits>

#include "value_utils.h"

namespace json {
namespace {
bool space(int c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
bool digit(int c) { return c >= '0' && c <= '9'; }
bool delimiter(int c) {
  return c == -1 || space(c) || c == ',' || c == ']' || c == '}';
}
int hex(int c) {
  if (digit(c)) return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
// Shared number grammar for input and output; no floating-point conversion,
// locale, overflow, NaN or permissive C-library parsing is involved.
class Number {
 public:
  bool accept(int c) {
    switch (state_) {
      case State::kFirst:
        if (c == '-')
          state_ = State::kSign;
        else
          return initial(c);
        return true;
      case State::kSign: return initial(c);
      case State::kZero:
      case State::kInteger:
        if (digit(c)) {
          if (state_ == State::kZero) return false;
          return true;
        }
        if (c == '.')
          state_ = State::kDot;
        else if (c == 'e' || c == 'E')
          state_ = State::kExponent;
        else
          return false;
        return true;
      case State::kDot:
        if (!digit(c)) return false;
        state_ = State::kFraction;
        return true;
      case State::kFraction:
        if (digit(c)) return true;
        if (c != 'e' && c != 'E') return false;
        state_ = State::kExponent;
        return true;
      case State::kExponent:
        if (c == '+' || c == '-') {
          state_ = State::kExponentSign;
          return true;
        }
        [[fallthrough]];
      case State::kExponentSign:
        if (!digit(c)) return false;
        state_ = State::kExponentDigits;
        return true;
      case State::kExponentDigits: return digit(c);
    }
    return false;
  }
  bool complete() const {
    return state_ == State::kZero || state_ == State::kInteger ||
           state_ == State::kFraction || state_ == State::kExponentDigits;
  }

 private:
  enum class State : uint8_t {
    kFirst,
    kSign,
    kZero,
    kInteger,
    kDot,
    kFraction,
    kExponent,
    kExponentSign,
    kExponentDigits
  };
  State state_ = State::kFirst;
  bool initial(int c) {
    if (!digit(c)) return false;
    state_ = c == '0' ? State::kZero : State::kInteger;
    return true;
  }
};
struct Buffer {
  char* data;
  std::size_t capacity, used = 0;
  bool truncated = false;
  void append(uint8_t c) {
    if (data && used < capacity)
      data[used++] = static_cast<char>(c);
    else
      truncated = true;
  }
  void assign(Token& token) const {
    token.text = data ? std::string_view(data, used) : std::string_view{};
    token.truncated = truncated;
  }
};
}  // namespace
int Reader::peek() {
  if (look_ == -2) look_ = input_.read ? input_.read(input_.context) : -1;
  return look_;
}
int Reader::take() {
  const int c = peek();
  last_consumed_ = c >= 0;
  if (last_consumed_) ++position_;
  look_ = -2;
  return c;
}
int Reader::nonSpace() {
  while (space(peek())) take();
  return peek();
}
bool Reader::fail(Error error) {
  if (error_ == Error::kNone) {
    error_ = error;
    error_offset_ = look_ == -2 && last_consumed_ ? position_ - 1 : position_;
    if (input_.failed) input_.failed(error, error_offset_, input_.context);
  }
  return false;
}
bool Reader::hex4(uint16_t& value) {
  value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    const int n = hex(take());
    if (n < 0) return fail();
    value = static_cast<uint16_t>((value << 4) | n);
  }
  return true;
}
bool Reader::string(Token& token, char* bytes, std::size_t capacity,
                    Output decoded) {
  if (take() != '"') return fail();
  Buffer out{bytes, capacity};
  const auto append = [&](uint8_t value) {
    token.contains_nul |= value == 0;
    out.append(value);
    const char byte = static_cast<char>(value);
    return !decoded.write || decoded.write({&byte, 1}, decoded.context) ||
           fail(Error::kOutput);
  };
  values::Utf8 utf8;
  for (;;) {
    int c = take();
    if (c == '"') {
      if (!utf8.complete()) return fail();
      out.assign(token);
      return true;
    }
    if (c < 0x20 || c > 255) return fail();
    if (c != '\\') {
      if (!utf8.accept(static_cast<uint8_t>(c))) return fail();
      if (!append(static_cast<uint8_t>(c))) return false;
      continue;
    }
    if (!utf8.complete()) return fail();
    c = take();
    if (c == 'u') {
      uint16_t first = 0, second = 0;
      if (!hex4(first)) return false;
      uint32_t point = first;
      if (first >= 0xd800 && first <= 0xdbff) {
        if (take() != '\\' || take() != 'u' || !hex4(second) ||
            second < 0xdc00 || second > 0xdfff)
          return fail();
        point = 0x10000 + ((uint32_t(first) - 0xd800) << 10) + second - 0xdc00;
      } else if (first >= 0xdc00 && first <= 0xdfff)
        return fail();
      if (point < 0x80) {
        if (!append(static_cast<uint8_t>(point))) return false;
      } else if (point < 0x800) {
        if (!append(static_cast<uint8_t>(0xc0 | (point >> 6))) ||
            !append(static_cast<uint8_t>(0x80 | (point & 63))))
          return false;
      } else if (point < 0x10000) {
        if (!append(static_cast<uint8_t>(0xe0 | (point >> 12))) ||
            !append(static_cast<uint8_t>(0x80 | ((point >> 6) & 63))) ||
            !append(static_cast<uint8_t>(0x80 | (point & 63))))
          return false;
      } else {
        if (!append(static_cast<uint8_t>(0xf0 | (point >> 18))) ||
            !append(static_cast<uint8_t>(0x80 | ((point >> 12) & 63))) ||
            !append(static_cast<uint8_t>(0x80 | ((point >> 6) & 63))) ||
            !append(static_cast<uint8_t>(0x80 | (point & 63))))
          return false;
      }
    } else {
      switch (c) {
        case '"':
        case '\\':
        case '/': break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        default: return fail();
      }
      if (!append(static_cast<uint8_t>(c))) return false;
    }
  }
}
bool Reader::number(Token& token, char* bytes, std::size_t capacity) {
  Buffer out{bytes, capacity};
  Number number;
  while (!delimiter(peek())) {
    const int c = take();
    if (!number.accept(c)) return fail();
    out.append(static_cast<uint8_t>(c));
  }
  if (!number.complete()) return fail();
  out.assign(token);
  return true;
}
bool Reader::literal(const char* text) {
  while (*text)
    if (take() != *text++) return fail();
  return delimiter(peek()) || fail();
}
bool Reader::value(Token& out, char* buffer, std::size_t capacity,
                   Output decoded) {
  const int c = nonSpace();
  if (c == '{' || c == '[') {
    if (depth_ == frames_.size()) return fail(Error::kDepth);
    take();
    frames_[depth_++] = c == '{' ? State::kObjectFirst : State::kArrayFirst;
    out.kind = c == '{' ? Kind::kObject : Kind::kArray;
    return true;
  }
  if (c == '"') {
    out.kind = Kind::kString;
    return string(out, buffer, capacity, decoded);
  }
  if (c == '-' || digit(c)) {
    out.kind = Kind::kNumber;
    return number(out, buffer, capacity);
  }
  if (c == 't') {
    out.kind = Kind::kTrue;
    return literal("true");
  }
  if (c == 'f') {
    out.kind = Kind::kFalse;
    return literal("false");
  }
  if (c == 'n') {
    out.kind = Kind::kNull;
    return literal("null");
  }
  return fail();
}
bool Reader::next(Token& out, char* buffer, std::size_t capacity,
                  Output decoded) {
  out = {};
  if (error_ != Error::kNone) return false;
  if (ended_) return true;
  for (;;) {
    const int c = nonSpace();
    if (!depth_) {
      if (root_done_) {
        if (c != -1) return fail();
        ended_ = true;
        return true;
      }
      root_done_ = true;
      return value(out, buffer, capacity, decoded);
    }
    auto& state = frames_[depth_ - 1];
    switch (state) {
      case State::kObjectFirst:
      case State::kObjectKey:
        if (state == State::kObjectFirst && c == '}') {
          take();
          --depth_;
          out.kind = Kind::kEndObject;
          return true;
        }
        if (c != '"') return fail();
        state = State::kObjectColon;
        out.kind = Kind::kKey;
        return string(out, buffer, capacity, decoded);
      case State::kObjectColon:
        if (take() != ':') return fail();
        state = State::kObjectValue;
        break;
      case State::kObjectValue:
        state = State::kObjectAfter;
        return value(out, buffer, capacity, decoded);
      case State::kArrayFirst:
      case State::kArrayValue:
        if (state == State::kArrayFirst && c == ']') {
          take();
          --depth_;
          out.kind = Kind::kEndArray;
          return true;
        }
        state = State::kArrayAfter;
        return value(out, buffer, capacity, decoded);
      case State::kObjectAfter:
      case State::kArrayAfter: {
        const bool object = state == State::kObjectAfter;
        if (c == (object ? '}' : ']')) {
          take();
          --depth_;
          out.kind = object ? Kind::kEndObject : Kind::kEndArray;
          return true;
        }
        if (take() != ',') return fail();
        state = object ? State::kObjectKey : State::kArrayValue;
        break;
      }
    }
  }
}
bool Reader::skip(const Token& current) {
  if (current.kind != Kind::kObject && current.kind != Kind::kArray)
    return true;
  unsigned depth = 1;
  while (depth) {
    Token token;
    if (!next(token)) return false;
    if (token.kind == Kind::kObject || token.kind == Kind::kArray)
      ++depth;
    else if (token.kind == Kind::kEndObject || token.kind == Kind::kEndArray)
      --depth;
    else if (token.kind == Kind::kEnd)
      return fail();
  }
  return true;
}
bool Reader::finish() {
  Token end;
  return next(end) && end.kind == Kind::kEnd;
}
int Fields::accept(const Token& key) {
  if (key.kind != Kind::kKey || key.truncated || count_ > 64) return -1;
  for (std::size_t i = 0; i < count_; ++i) {
    if (key.text != names_[i]) continue;
    const auto bit = uint64_t{1} << i;
    if (seen_ & bit) return -2;
    seen_ |= bit;
    return static_cast<int>(i);
  }
  return -1;
}
bool Writer::fail(Error error) {
  if (error_ == Error::kNone) error_ = error;
  return false;
}
bool Writer::emit(std::string_view bytes) {
  if (error_ != Error::kNone) return false;
  return (output_.write && output_.write(bytes, output_.context)) ||
         fail(Error::kOutput);
}
bool Writer::beforeValue() {
  if (string_open_) return fail(Error::kUsage);
  if (!depth_) {
    if (root_done_) return fail(Error::kUsage);
    root_done_ = true;
    return true;
  }
  auto& frame = frames_[depth_ - 1];
  if (frame.object) {
    if (!frame.needs_value) return fail(Error::kUsage);
    frame.needs_value = false;
    return true;
  }
  const bool first = frame.first;
  frame.first = false;
  return first || emit(",");
}
bool Writer::container(bool object) {
  if (depth_ == frames_.size()) return fail(Error::kDepth);
  if (!beforeValue() || !emit(object ? "{" : "[")) return false;
  frames_[depth_++] = {object, true, false};
  return true;
}
bool Writer::object() { return container(true); }
bool Writer::array() { return container(false); }
bool Writer::end() {
  if (string_open_ || !depth_ || frames_[depth_ - 1].needs_value)
    return fail(Error::kUsage);
  return emit(frames_[--depth_].object ? "}" : "]");
}
bool Writer::quoted(std::string_view value) {
  values::Utf8 check;
  for (uint8_t c : value)
    if (!check.accept(c)) return fail(Error::kUsage);
  if (!check.complete()) return fail(Error::kUsage);
  return emit("\"") && quotedContent(value) && emit("\"");
}
bool Writer::quotedContent(std::string_view value) {
  std::size_t start = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (c >= 0x20 && c != '"' && c != '\\') continue;
    if (!emit(value.substr(start, i - start))) return false;
    if (c == '"' || c == '\\') {
      const char escaped[] = {'\\', static_cast<char>(c)};
      if (!emit({escaped, sizeof(escaped)})) return false;
    } else {
      constexpr char digits[] = "0123456789abcdef";
      const char escaped[] = {'\\',           'u',           '0', '0',
                              digits[c >> 4], digits[c & 15]};
      if (!emit({escaped, sizeof(escaped)})) return false;
    }
    start = i + 1;
  }
  return emit(value.substr(start));
}
bool Writer::key(std::string_view value) {
  if (string_open_ || !depth_) return fail(Error::kUsage);
  auto& frame = frames_[depth_ - 1];
  if (!frame.object || frame.needs_value) return fail(Error::kUsage);
  if ((!frame.first && !emit(",")) || !quoted(value) || !emit(":"))
    return false;
  frame.first = false;
  frame.needs_value = true;
  return true;
}
bool Writer::string(std::string_view value) {
  return beforeValue() && quoted(value);
}
bool Writer::beginString() {
  if (!beforeValue() || !emit("\"")) return false;
  string_utf8_ = {};
  string_open_ = true;
  return true;
}
bool Writer::stringChunk(std::string_view value) {
  if (!string_open_) return fail(Error::kUsage);
  for (const uint8_t byte : value)
    if (!string_utf8_.accept(byte)) return fail(Error::kUsage);
  return quotedContent(value);
}
bool Writer::endString() {
  if (!string_open_ || !string_utf8_.complete()) return fail(Error::kUsage);
  if (!emit("\"")) return false;
  string_open_ = false;
  return true;
}
bool Writer::streamedString(bool (*source)(Output, void*), void* context) {
  if (!source) return fail(Error::kUsage);
  if (!beginString()) return false;
  if (!source({this,
               [](std::string_view bytes, void* raw) {
                 return static_cast<Writer*>(raw)->stringChunk(bytes);
               }},
              context))
    return fail(Error::kOutput);
  return endString();
}
bool Writer::number(std::string_view value) {
  Number number;
  for (uint8_t c : value)
    if (!number.accept(c)) return fail(Error::kUsage);
  if (!number.complete()) return fail(Error::kUsage);
  return beforeValue() && emit(value);
}
bool Writer::integer(uint64_t value) {
  char buffer[20];
  auto at = sizeof(buffer);
  do {
    buffer[--at] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value);
  return beforeValue() && emit({buffer + at, sizeof(buffer) - at});
}
bool Writer::signedInteger(int64_t value) {
  if (value >= 0) return integer(static_cast<uint64_t>(value));
  const uint64_t magnitude = uint64_t{0} - static_cast<uint64_t>(value);
  char buffer[21];
  auto at = sizeof(buffer);
  auto remaining = magnitude;
  do {
    buffer[--at] = static_cast<char>('0' + remaining % 10);
    remaining /= 10;
  } while (remaining);
  buffer[--at] = '-';
  return beforeValue() && emit({buffer + at, sizeof(buffer) - at});
}
bool Writer::boolean(bool value) {
  return beforeValue() && emit(value ? "true" : "false");
}
bool Writer::null() { return beforeValue() && emit("null"); }
bool Writer::finish() {
  if (string_open_) return fail(Error::kUsage);
  return (error_ == Error::kNone && root_done_ && !depth_) ||
         fail(Error::kUsage);
}
bool exactInteger(std::string_view text, int64_t& output) noexcept {
  if (text.empty() || text.size() > SIZE_MAX - 20) return false;
  const bool negative = text.front() == '-';
  if (negative) text.remove_prefix(1);
  const auto exponent_at = text.find_first_of("eE");
  auto mantissa = text.substr(0, exponent_at);
  const auto dot = mantissa.find('.');
  const std::size_t fraction =
      dot == std::string_view::npos ? 0 : mantissa.size() - dot - 1;
  std::size_t exponent = 0;
  bool exponent_negative = false;
  if (exponent_at != std::string_view::npos) {
    auto digits = text.substr(exponent_at + 1);
    if (!digits.empty() && (digits.front() == '-' || digits.front() == '+')) {
      exponent_negative = digits.front() == '-';
      digits.remove_prefix(1);
    }
    if (digits.empty()) return false;
    // Beyond this derived magnitude a nonzero bounded mantissa cannot fit
    // int64 or cancel its fractional digits. Saturation also accepts0e99999.
    const auto cap = text.size() + 20;
    for (const char digit : digits) {
      if (digit < '0' || digit > '9') return false;
      const auto value = static_cast<unsigned>(digit - '0');
      exponent = exponent > (cap - value) / 10 ? cap : exponent * 10 + value;
    }
  }
  bool nonzero = false;
  unsigned dots = 0;
  std::size_t digits = 0;
  for (const char digit : mantissa) {
    if (digit == '.') {
      if (++dots > 1) return false;
      continue;
    }
    if (digit < '0' || digit > '9') return false;
    ++digits;
    nonzero |= digit != '0';
  }
  if (!digits) return false;
  if (!nonzero) {
    output = 0;
    return true;
  }
  std::size_t drop = 0, append = 0;
  if (exponent_negative) {
    if (exponent > digits || fraction > digits - exponent) return false;
    drop = fraction + exponent;
  } else if (exponent >= fraction)
    append = exponent - fraction;
  else
    drop = fraction - exponent;
  if (append > 18) return false;
  while (drop) {
    if (!mantissa.empty() && mantissa.back() == '.') mantissa.remove_suffix(1);
    if (mantissa.empty() || mantissa.back() != '0') return false;
    mantissa.remove_suffix(1);
    --drop;
  }
  constexpr uint64_t kNegativeLimit = uint64_t{1} << 63;
  const uint64_t limit = negative ? kNegativeLimit : kNegativeLimit - 1;
  uint64_t value = 0;
  auto add = [&](unsigned digit) {
    if (value > (limit - digit) / 10) return false;
    value = value * 10 + digit;
    return true;
  };
  for (const char digit : mantissa)
    if (digit != '.' && !add(static_cast<unsigned>(digit - '0'))) return false;
  while (append--)
    if (!add(0)) return false;
  output = negative ? value == kNegativeLimit ? INT64_MIN
                                              : -static_cast<int64_t>(value)
                    : static_cast<int64_t>(value);
  return true;
}

}  // namespace json
