// The single streaming parser kernel. HTTP and host replay supply the same
// small Stream interface: peek/read/readBytes/endAfterNumber/nonSpace/complete.
#pragma once

#include "provider_json.h"

namespace football {
namespace json_detail {
inline bool space(int c) {
  return c == ' ' || c == '\r' || c == '\n' || c == '\t';
}

// Every consumed byte passes this bounded guard, including bytes which the
// underlying JSON library skips because of an item filter. It performs no
// reads of its own and retains no provider values.
template <class Stream>
class CheckedStream {
 public:
  explicit CheckedStream(Stream& source) : source_(source) {}
  int peek() { return syntax_.valid() ? source_.peek() : -1; }
  int read() {
    if (!syntax_.valid()) return -1;
    const int c = source_.read();
    if (c >= 0) syntax_.accept(c);
    return syntax_.valid() ? c : -1;
  }
  void endAfterNumber(bool on) { source_.endAfterNumber(on); }
  std::size_t readBytes(char* out, std::size_t count) {
    std::size_t n = 0;
    for (; n < count; ++n) {
      const int c = read();
      if (c < 0) break;
      out[n] = static_cast<char>(c);
    }
    return n;
  }
  int nonSpace() {
    int c;
    do {
      c = read();
    } while (space(c));
    return c;
  }
  bool complete() { return source_.complete() && syntax_.complete(); }
  bool valid() const { return syntax_.valid(); }

 private:
  Stream& source_;
  Syntax syntax_;
};

template <class Stream>
class Reader {
 public:
  Reader(Stream& stream, const char* primary, cfg::Provider provider,
         JsonConsumer consumer, void* context, JsonAllocator& allocator,
         const bool* enough, const JsonObserver* observer, JsonScan& scan)
      : stream_(stream),
        primary_(primary),
        consumer_(consumer),
        context_(context),
        enough_(enough),
        observer_(observer),
        scan_(scan),
        filter_(&allocator),
        value_(&allocator),
        primary_filter_(jsonElementFields(provider, primary)) {}

  bool read(bool optional) {
    if (consumer_ == nullptr || !validJsonObserver(observer_, primary_)) {
      scan_.invalid_configuration = true;
      return false;
    }
    bool ok;
    if (primary_ == nullptr) {
      ok = parse(cfg::kJsonDepth, nullptr) &&
           consumer_(value_.template as<ArduinoJson::JsonVariantConst>(),
                     context_);
      value_.clear();
      found_ = ok;
    } else {
      const uint16_t candidates = observer_ && observer_->count
                                      ? (uint16_t{1} << observer_->count) - 1
                                      : 0;
      ok = walk(0, 0, candidates, *primary_ == 0);
    }
    if (!ok || (!found_ && !optional)) return false;
    if (stopped()) {
      scan_.early_stopped = true;
      return true;
    }
    if (stream_.nonSpace() != -1 || !stream_.complete()) return false;
    scan_.document_complete = true;
    return true;
  }

 private:
  bool stopped() const { return enough_ && *enough_; }
  int peekValue() {
    while (space(stream_.peek())) stream_.read();
    return stream_.peek();
  }
  // The sole current-value document is cleared before changing the sole
  // active filter. Even alternating selectors never retain multiple masks.
  bool parse(unsigned nesting, const char* fields) {
    value_.clear();
    if (fields != active_filter_) {
      filter_.clear();
      active_filter_ = nullptr;
      if (fields) {
        const auto err = deserializeJson(filter_, fields);
        if (err) {
          scan_.allocation_failed |=
              err == ArduinoJson::DeserializationError::NoMemory;
          return false;
        }
        active_filter_ = fields;
      }
    }
    const int c = peekValue();
    // ArduinoJson otherwise consumes the separator after a scalar number.
    // The Stream holds that separator for the surrounding wrapper walk.
    stream_.endAfterNumber(c == '-' || (c >= '0' && c <= '9'));
    const auto err =
        fields ? deserializeJson(
                     value_, stream_,
                     ArduinoJson::DeserializationOption::Filter(
                         filter_.template as<ArduinoJson::JsonVariantConst>()),
                     ArduinoJson::DeserializationOption::NestingLimit(nesting))
               : deserializeJson(
                     value_, stream_,
                     ArduinoJson::DeserializationOption::NestingLimit(nesting));
    stream_.endAfterNumber(false);
    scan_.allocation_failed |=
        err == ArduinoJson::DeserializationError::NoMemory;
    return !err;
  }
  static bool startHasKind(int c, JsonValueKind kind) {
    switch (kind) {
      case JsonValueKind::kAny: return true;
      case JsonValueKind::kObject: return c == '{';
      case JsonValueKind::kArray: return c == '[';
      case JsonValueKind::kString: return c == '"';
      case JsonValueKind::kBoolean: return c == 't' || c == 'f';
      case JsonValueKind::kNumber: return c == '-' || (c >= '0' && c <= '9');
    }
    return false;
  }
  bool deliver(int field, unsigned nesting) {
    const auto* descriptor = field < 0 ? nullptr : &observer_->fields[field];
    if (descriptor && !startHasKind(peekValue(), descriptor->kind)) {
      scan_.wrong_type |= uint16_t{1} << field;
      // Validate and skip the wrongly typed value without retaining an
      // unexpectedly large metadata object. The original caps still apply.
      const bool ok = parse(nesting, "false");
      value_.clear();
      return ok;
    }
    if (!parse(nesting, descriptor ? descriptor->filter : primary_filter_))
      return false;
    const auto value = value_.template as<ArduinoJson::JsonVariantConst>();
    bool ok = true;
    if (descriptor) {
      if (!jsonValueHasKind(value, descriptor->kind))
        scan_.wrong_type |= uint16_t{1} << field;
      else
        ok = observer_->consume(descriptor->tag, value, observer_->context);
    } else {
      ok = consumer_(value, context_);
    }
    value_.clear();
    return ok;
  }
  bool selected(int field, unsigned depth) {
    const bool primary = field < 0;
    if (primary) {
      scan_.primary_depth = static_cast<uint8_t>(depth);
      scan_.primary_duplicates |= found_;
    }
    const auto bit = primary ? uint16_t{0} : uint16_t{1} << field;
    if (!primary) {
      if (scan_.seen & bit) scan_.duplicates |= bit;
      scan_.seen |= bit;
    }
    const bool items = primary || observer_->fields[field].delivery ==
                                      JsonDelivery::kArrayItems;
    if (!items) {
      if (!deliver(field, cfg::kJsonDepth - depth)) return false;
    } else if (peekValue() != '[') {
      // Metadata shape errors are evidence failures, not score failures.
      if (primary) return false;
      scan_.wrong_type |= bit;
      if (!parse(cfg::kJsonDepth - depth, "false")) return false;
      value_.clear();
    } else {
      if (primary) found_ = true;
      stream_.read();
      if (peekValue() == ']') {
        stream_.read();
      } else {
        for (;;) {
          if (!deliver(field, cfg::kJsonDepth - depth)) return false;
          if (stopped()) return true;
          const int c = stream_.nonSpace();
          if (c == ']') break;
          if (c != ',') return false;
        }
      }
    }
    if (!primary) scan_.completed |= bit;
    return true;
  }
  void beginField(int field) {
    const auto bit = uint16_t{1} << field;
    if (scan_.seen & bit) scan_.duplicates |= bit;
    scan_.seen |= bit;
  }
  bool finishCount(int field, uint32_t count) {
    if (field < 0) return true;
    value_.clear();
    value_.set(count);
    const bool ok =
        observer_->consume(observer_->fields[field].tag,
                           value_.template as<ArduinoJson::JsonVariantConst>(),
                           observer_->context);
    value_.clear();
    if (ok) scan_.completed |= uint16_t{1} << field;
    return ok;
  }
  bool walk(unsigned depth, uint8_t path_depth, uint16_t candidates,
            bool primary_selected = false, bool array_element = false) {
    if (depth > cfg::kJsonDepth) return false;
    const int value_start = peekValue();
    // Anchored paths start in an object. A malformed ancestor leaves its
    // descendant fields absent, so it cannot accidentally establish proof.
    if (path_depth == 0 && value_start != '{') candidates = 0;
    if (path_depth != 0) {
      for (uint8_t i = 0; candidates && i < observer_->count; ++i) {
        const auto bit = uint16_t{1} << i;
        if (!(candidates & bit)) continue;
        const auto& field = observer_->fields[i];
        const bool terminal = field.path_size == path_depth;
        const bool array =
            field.array_parents & (uint16_t{1} << (path_depth - 1));
        if (array_element) {
          // Indices disappear only inside explicitly marked array parents;
          // each nonterminal element still has to provide an object key.
          if (!terminal && value_start != '{') candidates &= ~bit;
        } else if ((array && value_start != '[') ||
                   (!array && !terminal && value_start != '{')) {
          candidates &= ~bit;
        }
      }
    }
    if (primary_selected) return selected(-1, depth);
    int count_field = -1;
    for (uint8_t i = 0; candidates && i < observer_->count; ++i) {
      const auto bit = uint16_t{1} << i;
      if ((candidates & bit) && observer_->fields[i].path_size == path_depth) {
        if (observer_->fields[i].delivery != JsonDelivery::kArrayCount &&
            observer_->fields[i].delivery != JsonDelivery::kObjectCount)
          return selected(i, depth);
        count_field = i;
        beginField(i);
        candidates &= ~bit;
      }
    }
    const int count_start =
        count_field >= 0 && observer_->fields[count_field].delivery ==
                                JsonDelivery::kObjectCount
            ? '{'
            : '[';
    if (count_field >= 0 && value_start != count_start) {
      scan_.wrong_type |= uint16_t{1} << count_field;
      if (!parse(cfg::kJsonDepth - depth, "false")) return false;
      value_.clear();
      scan_.completed |= uint16_t{1} << count_field;
      return true;
    }
    if (value_start == '[') {
      stream_.read();
      uint32_t count = 0;
      if (peekValue() == ']') {
        stream_.read();
        return finishCount(count_field, 0);
      }
      for (;;) {
        if (!walk(depth + 1, path_depth, candidates, false, true)) return false;
        if (stopped()) return true;
        if (count == UINT32_MAX) return false;
        ++count;
        const int c = stream_.nonSpace();
        if (c == ']') return finishCount(count_field, count);
        if (c != ',') return false;
      }
    }
    if (value_start == '{') {
      stream_.read();
      uint32_t count = 0;
      int c = stream_.nonSpace();
      if (c == '}') return finishCount(count_field, 0);
      for (;;) {
        if (c != '"') return false;
        std::size_t n = 0;
        bool same_primary = true;
        uint16_t matching = candidates;
        // Compare key bytes directly, without a per-recursion key/path buffer.
        // Existing rejection of escaped/overlong wrapper keys is preserved.
        while ((c = stream_.read()) != '"') {
          if (c < 0 || c == '\\' || c < 32 || n >= kJsonWrapperKeyBytes)
            return false;
          same_primary =
              same_primary && static_cast<unsigned char>(primary_[n]) == c;
          for (uint8_t i = 0; matching && i < observer_->count; ++i) {
            const auto bit = uint16_t{1} << i;
            if ((matching & bit) &&
                static_cast<unsigned char>(
                    observer_->fields[i].path[path_depth][n]) != c)
              matching &= ~bit;
          }
          ++n;
        }
        for (uint8_t i = 0; matching && i < observer_->count; ++i) {
          const auto bit = uint16_t{1} << i;
          if ((matching & bit) && observer_->fields[i].path[path_depth][n])
            matching &= ~bit;
        }
        if (stream_.nonSpace() != ':') return false;
        if (!walk(depth + 1, path_depth + 1, matching,
                  same_primary && primary_[n] == 0))
          return false;
        if (stopped()) return true;
        if (count == UINT32_MAX) return false;
        ++count;
        c = stream_.nonSpace();
        if (c == '}') return finishCount(count_field, count);
        if (c != ',') return false;
        c = stream_.nonSpace();
      }
    }
    // Unselected scalars still use ArduinoJson, so malformed ignored values
    // cannot turn a partial/invalid document into a completeness proof.
    const bool ok = parse(cfg::kJsonDepth - depth, nullptr);
    value_.clear();
    return ok;
  }
  Stream& stream_;
  const char* primary_;
  JsonConsumer consumer_;
  void* context_;
  const bool* enough_;
  const JsonObserver* observer_;
  JsonScan& scan_;
  ArduinoJson::JsonDocument filter_;
  ArduinoJson::JsonDocument value_;
  const char* primary_filter_;
  const char* active_filter_ = nullptr;
  bool found_ = false;
};
}  // namespace json_detail

// Returns true only for an accepted primary response (or optional absence).
// An intentional primary-consumer shortcut is successful but never complete.
// Body/transport limits belong to Stream and are checked by complete().
// Observer values are transient; callbacks must copy only bounded needed data.
template <class Stream>
bool readJson(Stream& stream, const char* array_key, cfg::Provider provider,
              JsonConsumer consumer, void* context, JsonAllocator& allocator,
              bool optional = false, const bool* enough = nullptr,
              const JsonObserver* observer = nullptr,
              JsonScan* scan = nullptr) {
  JsonScan local;
  JsonScan& result = scan ? *scan : local;
  result = {};
  json_detail::CheckedStream<Stream> checked(stream);
  json_detail::Reader<json_detail::CheckedStream<Stream>> reader(
      checked, array_key, provider, consumer, context, allocator, enough,
      observer, result);
  return reader.read(optional) && checked.valid();
}
}  // namespace football
