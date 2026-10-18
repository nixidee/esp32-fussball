// One bounded JSON grammar. Transport owns byte limits/deadlines; typed
// consumers own schemas, duplicate detection and publication. No DOM or heap.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "app_config.h"
#include "value_utils.h"

namespace json {
enum class Error : uint8_t { kNone, kSyntax, kDepth, kOutput, kUsage };
struct ErrorInfo {
  Error error = Error::kNone;
  // Zero-based byte offset in the original UTF-8 document; EOF is its length.
  std::size_t offset = 0;
};
struct Input {
  void* context;
  // Return an unsigned byte, or -1 at end. Transport failure must additionally
  // be checked by its owner; end of a complete JSON value alone is not I/O
  // proof.
  int (*read)(void*);
  // Optional transport diagnostics; called once at the first grammar failure.
  void (*failed)(Error, std::size_t offset, void*) = nullptr;
};
struct Output {
  void* context;
  bool (*write)(std::string_view, void*);
};
enum class Kind : uint8_t {
  kObject,
  kArray,
  kEndObject,
  kEndArray,
  kKey,
  kString,
  kNumber,
  kTrue,
  kFalse,
  kNull,
  kEnd
};
struct Token {
  Kind kind = Kind::kEnd;
  // Borrowed consumer buffer; embedded NUL is represented by length.
  std::string_view text;
  // A valid oversized token was fully checked but only its prefix retained.
  // Unknown provider fields can be discarded without a string-size ceiling.
  bool truncated = false;
  // True for U+0000 anywhere in a decoded key/string, including a suffix
  // beyond the retained prefix. JSON permits it; typed consumers decide.
  bool contains_nul = false;
};
// Convert a complete, grammar-validated JSON number to an exact signed integer.
// Integral decimal/exponent forms are accepted; floating-point rounding never
// turns a fractional value into an identity/score. Output is unchanged on
// error.
bool exactInteger(std::string_view text, int64_t& output) noexcept;

class Reader {
 public:
  explicit Reader(Input input) : input_(input) {}
  // Reuses the supplied buffer for key/string/number text. A consumer may use
  // a zero-sized buffer to validate/skip; truncation never skips syntax checks.
  // An optional sink additionally receives every decoded byte of keys/strings,
  // independent of retained prefix capacity. Chunks may split UTF-8 code
  // points; the grammar validates the complete string before success. The
  // consumer owns transactional scratch: cancellation or later syntax failure
  // can follow already delivered bytes and must not publish partial data.
  bool next(Token& out, char* buffer = nullptr, std::size_t capacity = 0,
            Output decoded = {});
  bool skip(const Token& current);
  bool finish();
  Error error() const { return error_; }
  std::size_t errorOffset() const { return error_offset_; }

 private:
  enum class State : uint8_t {
    kObjectFirst,
    kObjectKey,
    kObjectColon,
    kObjectValue,
    kObjectAfter,
    kArrayFirst,
    kArrayValue,
    kArrayAfter
  };
  Input input_;
  std::array<State, cfg::kJsonDepth> frames_{};
  uint8_t depth_ = 0;
  int look_ = -2;
  Error error_ = Error::kNone;
  bool root_done_ = false, ended_ = false, last_consumed_ = false;
  std::size_t position_ = 0, error_offset_ = 0;
  int peek();
  int take();
  int nonSpace();
  bool fail(Error error = Error::kSyntax);
  bool value(Token&, char*, std::size_t, Output);
  bool string(Token&, char*, std::size_t, Output);
  bool number(Token&, char*, std::size_t);
  bool literal(const char* text);
  bool hex4(uint16_t& value);
};

// A schema helper for declared fields. Unknown and duplicate keys are separate
// failures. Field count is explicit and limited to64; no arbitrary-key table.
class Fields {
 public:
  explicit Fields(const char* const* names, std::size_t count)
      : names_(names), count_(count) {}
  // -1 unknown/truncated key, -2 duplicate, otherwise declared field index.
  int accept(const Token& key);
  bool contains(unsigned field) const {
    return field < 64 && (seen_ & (uint64_t{1} << field));
  }
  bool complete(uint64_t required) const {
    return (seen_ & required) == required;
  }

 private:
  const char* const* names_;
  std::size_t count_;
  uint64_t seen_ = 0;
};

class Writer {
 public:
  explicit Writer(Output output) : output_(output) {}
  bool object();
  bool array();
  bool end();
  bool key(std::string_view value);
  bool string(std::string_view value);
  // Source pushes arbitrary chunks through the supplied sink, then returns.
  // UTF-8 validation spans chunk boundaries; no whole string is retained.
  bool streamedString(bool (*source)(Output chunks, void*), void* context);
  // A producer may push one decoded string without retaining it. No other
  // key/value/container is accepted until endString proves complete UTF-8.
  bool beginString();
  bool stringChunk(std::string_view);
  bool endString();
  bool number(std::string_view value);
  bool integer(uint64_t value);
  bool signedInteger(int64_t value);
  bool boolean(bool value);
  bool null();
  // Convenience for typed scalar object fields; uses the same encoder and
  // checks the same grammar/state/output result as key()+value().
  bool field(std::string_view key, std::string_view value) {
    return this->key(key) && string(value);
  }
  bool field(std::string_view key, const char* value) {
    return this->key(key) && (value ? string(value) : null());
  }
  bool field(std::string_view key, std::nullptr_t) {
    return this->key(key) && null();
  }
  template <class T>
    requires std::is_integral_v<T>
  bool field(std::string_view key, T value) {
    if (!this->key(key)) return false;
    if constexpr (std::is_same_v<T, bool>)
      return boolean(value);
    else if constexpr (std::is_signed_v<T>)
      return signedInteger(value);
    else
      return integer(value);
  }
  bool finish();
  Error error() const { return error_; }

 private:
  struct Frame {
    bool object, first, needs_value;
  };
  Output output_;
  std::array<Frame, cfg::kJsonDepth> frames_{};
  uint8_t depth_ = 0;
  Error error_ = Error::kNone;
  bool root_done_ = false;
  values::Utf8 string_utf8_;
  bool string_open_ = false;
  bool fail(Error);
  bool emit(std::string_view);
  bool beforeValue();
  bool quoted(std::string_view);
  bool quotedContent(std::string_view);
  bool container(bool object);
};
}  // namespace json
