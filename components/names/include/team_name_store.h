// Bounded, versioned adopted club names. No global club registry or
// allocations.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "app_config.h"

namespace football::names::store {
inline constexpr uint16_t kNone = 0xffff;
inline constexpr std::size_t kKeyBytes = 63;
inline constexpr std::size_t kHeaderBytes = 96;
inline constexpr std::size_t kCheckpoint = 16;
using ReadAt = bool (*)(std::size_t offset, void*, std::size_t, void*);
using WriteAt = bool (*)(std::size_t offset, const void*, std::size_t, void*);
using Write = bool (*)(const void*, std::size_t, void*);
struct IO {
  void* context = nullptr;
  ReadAt read = nullptr;
  WriteAt write = nullptr;
};
struct Counts {
  uint16_t clubs = 0, aliases = 0, exceptions = 0;
  std::array<uint16_t, cfg::kProviderCount> bindings{};
};
struct Field {
  bool present = false;
  std::string_view value{};
};
struct Fields {
  Field de, en, short_name, league;
};
// References are local to exactly one checked generation. Zero means absent;
// a present empty field has a nonzero reference to a zero-length pool entry.
struct Club {
  uint16_t de = 0, en = 0, short_name = 0, league = 0;
  uint8_t present = 0;
};
struct Alias {
  uint16_t club = kNone, text = 0;
};
struct Binding {
  uint32_t id = 0;
  uint16_t club = kNone;
};
struct Exception {
  cfg::Provider provider{};
  uint32_t id = 0;
  uint16_t text = 0;
};
enum class Lookup : uint8_t { kFound, kMissing, kError };

class Reader {
 public:
  // Checks exact length, version, offsets, ordering, references, UTF-8 and CRC.
  // Failure invalidates this reader; it never modifies the source file.
  bool open(IO io, std::size_t exact_size);
  // Rebind a previously fully checked generation to a newly opened handle.
  // Requires an identical complete header receipt, including
  // size/generation/CRC. This is the normal refresh path; it never rescans the
  // global collection.
  bool reuse(IO io, const Reader& checked, std::size_t exact_size);
  void detach() { io_ = {}; }
  Counts counts() const;
  uint32_t generation() const;
  uint32_t digest() const;
  uint32_t sourceFingerprint() const;
  std::size_t size() const;
  bool key(uint16_t club, char (&out)[kKeyBytes + 1]) const;
  bool club(uint16_t index, Club& out) const;
  bool alias(uint16_t index, Alias& out) const;
  bool binding(cfg::Provider provider, uint16_t index, Binding& out) const;
  bool exception(uint16_t index, Exception& out) const;
  Lookup findKey(std::string_view key, uint16_t& club) const;
  Lookup findId(cfg::Provider provider, uint32_t id, uint16_t& club) const;
  Lookup findException(cfg::Provider provider, uint32_t id,
                       uint16_t& text) const;
  bool text(uint16_t reference, char* out, std::size_t capacity,
            std::size_t& length) const;
  bool streamText(uint16_t reference, Write write, void* context) const;
  // Checked pool cursor for streaming the shared-text JSON representation.
  // Zero means end/empty. nextText reports I/O failure separately from end.
  uint16_t firstText() const;
  bool nextText(uint16_t& reference) const;

 private:
  friend class Writer;
  IO io_{};
  std::array<uint8_t, kHeaderBytes> header_{};
  bool get(std::size_t at, void* out, std::size_t size) const;
  bool poolReference(uint16_t reference, std::size_t limit = SIZE_MAX) const;
};

class Writer {
 public:
  // The caller supplies an empty candidate, admitted independently of the old
  // committed file. No source/committed file is ever changed by this codec.
  bool begin(IO io, const Counts& counts, uint32_t generation,
             uint32_t source_fingerprint);
  // Ordered sections: complete sorted keys; sorted numeric IDs per provider
  // (providers in enum order); then club fields, aliases, and exceptions.
  // Club calls are file-local index order; aliases retain caller order.
  bool key(std::string_view value);
  bool binding(cfg::Provider provider, uint32_t id, uint16_t club);
  bool club(uint16_t index, const Fields& fields);
  bool alias(uint16_t club, std::string_view full_value);
  // Exceptions sorted by provider then ID; every identity survives
  // independently.
  bool exception(cfg::Provider provider, uint32_t id, std::string_view value);
  // Finalizes the header/CRC and validates by reading the entire candidate.
  // Returns zero on any error (including the complete-file byte quota).
  std::size_t finish();

 private:
  IO io_{};
  std::array<uint8_t, kHeaderBytes> header_{};
  char previous_key_[kKeyBytes + 1]{};
  uint32_t previous_id_ = 0, exception_id_ = 0;
  uint16_t cursor_ = 0, keys_ = 0, clubs_ = 0, aliases_ = 0, exceptions_ = 0;
  uint16_t binding_count_ = 0;
  uint8_t provider_ = 0, exception_provider_ = 0;
  bool sealed_ = false, failed_ = true;
  bool put(std::size_t at, const void* bytes, std::size_t size);
  bool append(const void* bytes, std::size_t size);
  bool seal();
  bool intern(std::string_view value, uint16_t& reference);
};

// Shared UTF-8 adoption rule. Invalid UTF-8 is rejected; a partial code point
// at the byte bound is omitted. Identity strings never pass through clipping.
bool adopt(std::string_view input, std::size_t byte_limit,
           std::string_view& prefix);
}  // namespace football::names::store
