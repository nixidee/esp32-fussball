// Bounded provider JSON protocol shared by HTTP and host response replay.
#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

#include "app_config.h"

namespace football {
// One unchanged budget for the active field mask and current value. No body
// document, per-field retained documents or production measurement state.
class JsonAllocator : public ArduinoJson::Allocator {
 public:
  void* allocate(std::size_t size) override;
  void deallocate(void* pointer) override;
  void* reallocate(void* pointer, std::size_t size) override;
  std::size_t used() const { return used_; }

 private:
  std::size_t used_ = 0;
};
using JsonConsumer = bool (*)(ArduinoJson::JsonVariantConst item,
                              void* context);

inline constexpr uint8_t kJsonObserverFields = 15;
inline constexpr std::size_t kJsonWrapperKeyBytes = 95;
enum class JsonDelivery : uint8_t {
  kValue,
  kArrayItems,
  kArrayCount,
  kObjectCount
};
enum class JsonValueKind : uint8_t {
  kAny,
  kObject,
  kArray,
  kString,
  kBoolean,
  kNumber
};
// A root-anchored sequence of object keys. Indices of marked arrays are
// transparent; unmarked nonterminal parents and array elements are objects.
// Bit i of array_parents requires the value at path[i] to be an array before
// walking its elements; use it for e.g. leagues[].season to reject malformed
// wrapper shapes. No path strings are constructed during traversal.
// kArrayItems requires an array at the final path; kind describes each item.
// kArrayCount/kObjectCount traverse a container and deliver its raw item/key
// count as a number. Object counts include duplicate keys before ArduinoJson
// can collapse them. These modes may overlap descendant paths, proving one
// common parent without retaining the container.
// kValue delivers one complete value of kind. filter is a small ArduinoJson
// filter applied only to that value/item, or nullptr. Paths must not overlap:
// a selected value/items subtree is parsed once and cannot have child
// observers.
struct JsonField {
  const char* const* path = nullptr;
  uint8_t path_size = 0;
  JsonDelivery delivery = JsonDelivery::kValue;
  JsonValueKind kind = JsonValueKind::kAny;
  const char* filter = nullptr;
  uint8_t tag = 0;
  uint16_t array_parents = 0;
};
using JsonFieldConsumer = bool (*)(uint8_t tag,
                                   ArduinoJson::JsonVariantConst value,
                                   void* context);
struct JsonObserver {
  const JsonField* fields = nullptr;
  uint8_t count = 0;
  JsonFieldConsumer consume = nullptr;
  void* context = nullptr;
};
// Bit i refers to observer.fields[i]. A duplicate is a second occurrence of
// the selected path, not a second item in one selected array. completed means
// the complete selected value/array was read; wrong_type remains separate.
// Missing, duplicated or wrongly typed metadata never invalidates primary
// scores. Consumers stage metadata/names and commit only after successful
// document_complete; early wizard completion cannot establish coverage.
struct JsonScan {
  uint16_t seen = 0;
  uint16_t completed = 0;
  uint16_t duplicates = 0;
  uint16_t wrong_type = 0;
  bool document_complete = false;
  bool early_stopped = false;
  bool allocation_failed = false;
  bool invalid_configuration = false;
  // Legacy score traversal still accepts a matching array inside wrappers.
  // Evidence consumers can require one root member (depth 1) explicitly.
  uint8_t primary_depth = 0;
  bool primary_duplicates = false;
};
static_assert(sizeof(JsonScan) <= 16);

// Existing ESPN primary item filters; unfiltered providers/lists return null.
const char* jsonElementFields(cfg::Provider provider, const char* array_key);
bool jsonValueHasKind(ArduinoJson::JsonVariantConst value, JsonValueKind kind);
bool validJsonObserver(const JsonObserver* observer, const char* array_key);

namespace json_detail {
// A bounded syntax guard for bytes already passing through this parser. It
// retains no values and consumes no input itself. ArduinoJson's filtered skip
// accepts some malformed tokens; those must never establish a complete proof.
class Syntax {
 public:
  void accept(int c);
  bool complete();
  bool valid() const { return valid_; }

 private:
  enum class Mode : uint8_t { kOutside, kString, kNumber, kLiteral };
  enum class Frame : uint8_t {
    kObjectFirst,
    kObjectKey,
    kObjectColon,
    kObjectValue,
    kObjectAfter,
    kArrayFirst,
    kArrayValue,
    kArrayAfter
  };
  enum class Number : uint8_t {
    kSign,
    kZero,
    kInteger,
    kDot,
    kFraction,
    kExponent,
    kExponentSign,
    kExponentDigits
  };
  void outside(int c);
  void string(int c);
  void number(int c);
  void finishValue();
  bool expectsValue() const;
  bool finishNumber();
  Frame frames_[cfg::kJsonDepth + 1]{};
  uint16_t unicode_ = 0;
  uint8_t depth_ = 0;
  Mode mode_ = Mode::kOutside;
  Number number_ = Number::kZero;
  uint8_t literal_ = 0, literal_position_ = 0;
  uint8_t escape_ = 0, unicode_digits_ = 0;
  uint8_t utf8_left_ = 0, utf8_low_ = 0x80, utf8_high_ = 0xbf;
  bool key_ = false, high_surrogate_ = false;
  bool root_done_ = false, valid_ = true;
};
static_assert(sizeof(Syntax) <= 48);
}  // namespace json_detail
}  // namespace football
