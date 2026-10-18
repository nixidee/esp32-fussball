// Typed Web schema helpers over the one shared JSON grammar. No DOM, heap or
// transport knowledge; candidates and full-document proof belong to callers.
#pragma once
#include <charconv>
#include <limits>
#include <type_traits>

#include "correlation_id.h"
#include "settings_json.h"
namespace web::detail {
using json::Kind;
using settings::FieldPath;
using settings::Reason;
struct Decoder {
  json::Reader& reader;
  settings::Violation* violation;
  Decoder(json::Reader& input, settings::Violation* invalid)
      : reader(input), violation(invalid) {}
  json::Token token;
  char text[cfg::kApiFootballKeyMaxBytes + 1]{};
  SettingsDecode result = SettingsDecode::kOk;
  bool next() {
    if (!reader.next(token, text, sizeof(text) - 1)) {
      result = SettingsDecode::kSyntax;
      return false;
    }
    text[token.text.size()] = 0;
    return true;
  }
  bool reject(const FieldPath& path, Reason reason) {
    if (violation) violation->set(path, reason);
    result = SettingsDecode::kViolation;
    return false;
  }
  template <std::size_t N, class Callback>
  bool object(const json::Token& first, const FieldPath& path,
              const char* const (&keys)[N], Callback callback,
              uint64_t required = 0) {
    if (first.kind != Kind::kObject) return reject(path, Reason::kType);
    json::Fields fields(keys, N);
    while (next()) {
      if (token.kind == Kind::kEndObject) {
        if (!fields.complete(required)) {
          for (unsigned i = 0; i < N; ++i)
            if ((required & (uint64_t{1} << i)) && !fields.contains(i))
              return reject(path.with(keys[i]), Reason::kRequired);
        }
        return true;
      }
      const int field = fields.accept(token);
      if (field < 0)
        return reject(path.with(text),
                      field == -2 ? Reason::kDuplicate : Reason::kUnknownKey);
      const auto at = path.with(keys[field]);
      if (!next() || !callback(static_cast<unsigned>(field), at)) return false;
    }
    return false;
  }
  template <class Callback>
  bool array(const json::Token& first, const FieldPath& path, std::size_t max,
             Callback callback, bool exact = false) {
    if (first.kind != Kind::kArray) return reject(path, Reason::kType);
    unsigned index = 0;
    while (next()) {
      if (token.kind == Kind::kEndArray)
        return !exact || index == max || reject(path, Reason::kRequired);
      if (index >= max) return reject(path, Reason::kTooMany);
      const auto at = path.withIndex(index);
      if (!callback(index++, at)) return false;
    }
    return false;
  }
  template <class T>
  bool scalar(T& out, const FieldPath& path, bool nullable = true) {
    if (token.kind == Kind::kNull)
      return nullable || reject(path, Reason::kRequired);
    if constexpr (std::is_same_v<T, bool>) {
      if (token.kind != Kind::kTrue && token.kind != Kind::kFalse)
        return reject(path, Reason::kType);
      out = token.kind == Kind::kTrue;
      return true;
    } else {
      static_assert(std::is_unsigned_v<T>);
      if (token.kind != Kind::kNumber) return reject(path, Reason::kType);
      if (token.truncated) return reject(path, Reason::kRange);
      if (token.text.find_first_of(".eE") != std::string_view::npos)
        return reject(path, Reason::kType);
      if (!token.text.empty() && token.text.front() == '-')
        return reject(path, Reason::kRange);
      uint64_t value = 0;
      const auto parsed = std::from_chars(
          token.text.data(), token.text.data() + token.text.size(), value);
      if (parsed.ec != std::errc{} || value > std::numeric_limits<T>::max())
        return reject(path, Reason::kRange);
      out = static_cast<T>(value);
      return true;
    }
  }
  bool correlation(uint64_t& out, const FieldPath& path) {
    if (token.kind != Kind::kString) return reject(path, Reason::kType);
    if (token.truncated || token.text.size() != 16)
      return reject(path, Reason::kRange);
    return parseCorrelationId(token.text, out) ||
           reject(path, Reason::kInvalid);
  }
  template <std::size_t N>
  bool bytes(settings::Bytes<N>& out, const FieldPath& path) {
    if (token.kind == Kind::kNull) return true;
    if (token.kind != Kind::kString) return reject(path, Reason::kType);
    return (!token.truncated && out.assign(token.text)) ||
           reject(path, Reason::kTooLong);
  }
};
}  // namespace web::detail
