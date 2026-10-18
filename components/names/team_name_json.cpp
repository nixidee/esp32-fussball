#include "team_name_json.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>

#include "binary_codec.h"
#include "football_model.h"
#include "value_utils.h"

namespace football::names {
namespace {
using ::json::Kind;
using settings::FieldPath;
using settings::Reason;
constexpr std::size_t kScratchBytes = 2 * cfg::kTeamNamesFileBytes;
std::size_t var(uint32_t value, uint8_t* out) {
  std::size_t n = 0;
  do {
    out[n++] = uint8_t(value & 127) | (value > 127 ? 128 : 0);
    value >>= 7;
  } while (value);
  return n;
}
uint32_t unvar(const uint8_t* data, std::size_t& at) {
  uint32_t value = 0;
  for (unsigned shift = 0;; shift += 7) {
    const auto byte = data[at++];
    value |= uint32_t(byte & 127) << shift;
    if (!(byte & 128)) return value;
  }
}
// Only normalized values live here. Sections move within one allocation; text
// references are pool-relative and club indices stay input-local until emit.
// Bindings already use sorted ID deltas. A valid FNS2 file needs <1.5 times its
// wire size here, including the final input->sorted index map: extra2/key,
// map2/club, and at most1/binding for a changed club-varint width. Their
// minimum wire costs are11/club and2/binding. Version2 additionally uses a
// four-byte text-ID map per distinct, referenced text. This and its associated
// pool/record bytes fit the shared2x arena. No full JSON or second file image
// exists.
struct Draft {
  enum Section : unsigned {
    kClubs,
    kKeys,
    kBindings,
    kAliases = kBindings + cfg::kProviderCount,
    kExceptions,
    kMap,
    kTexts,
    kPool,
    kSections
  };
  std::array<uint8_t, kScratchBytes> bytes;
  std::array<std::size_t, kSections> starts{}, sizes{};
  store::Counts counts;
  std::size_t used = 0;
  bool duplicate = false;
  uint8_t* data(unsigned part) { return bytes.data() + starts[part]; }
  const uint8_t* data(unsigned part) const {
    return bytes.data() + starts[part];
  }
  bool replace(unsigned part, std::size_t at, std::size_t old,
               const void* source, std::size_t length) {
    if (at > sizes[part] || old > sizes[part] - at ||
        length > bytes.size() - (used - old))
      return false;
    const auto position = starts[part] + at;
    std::memmove(bytes.data() + position + length,
                 bytes.data() + position + old, used - position - old);
    if (length && source)
      std::memcpy(bytes.data() + position, source, length);
    else if (length)
      std::memset(bytes.data() + position, 0, length);
    sizes[part] = sizes[part] - old + length;
    used = used - old + length;
    for (unsigned i = part + 1; i < kSections; ++i)
      starts[i] = starts[i] - old + length;
    return true;
  }
  bool append(unsigned part, const void* source, std::size_t length) {
    return replace(part, sizes[part], 0, source, length);
  }
  bool fits() const {
    std::size_t binding_count = 0;
    for (auto n : counts.bindings) binding_count += n;
    // Remove only staging padding; omitted wire checkpoints make this an
    // optimistic lower bound. Exact quota is checked by the existing Writer.
    return used - sizes[kMap] - sizes[kTexts] - 2 * counts.clubs -
               binding_count + store::kHeaderBytes <=
           cfg::kTeamNamesFileBytes;
  }
  bool addText(uint16_t id, uint16_t reference) {
    duplicate = false;
    std::size_t low = 0, high = sizes[kTexts] / 4;
    for (std::size_t at = 0; at < sizes[kTexts]; at += 4)
      if (binary::readU16(data(kTexts) + at + 2) == reference) {
        duplicate = true;
        return false;
      }
    while (low < high) {
      const auto middle = low + (high - low) / 2;
      const auto other = binary::readU16(data(kTexts) + middle * 4);
      if (id == other) {
        duplicate = true;
        return false;
      }
      if (id < other)
        high = middle;
      else
        low = middle + 1;
    }
    uint8_t row[4];
    binary::writeU16(row, id);
    binary::writeU16(row + 2, reference);
    return replace(kTexts, low * 4, 0, row, sizeof(row));
  }
  uint8_t* findText(uint16_t id) {
    std::size_t low = 0, high = sizes[kTexts] / 4;
    while (low < high) {
      const auto middle = low + (high - low) / 2;
      auto* row = data(kTexts) + middle * 4;
      const auto other = binary::readU16(row);
      if (id == other) return row + 2;
      if (id < other)
        high = middle;
      else
        low = middle + 1;
    }
    return nullptr;
  }
  std::string_view text(uint16_t reference) const {
    const auto at = std::size_t(reference - 1);
    return {reinterpret_cast<const char*>(data(kPool) + at + 2),
            binary::readU16(data(kPool) + at)};
  }
  bool keyAt(std::size_t& at, char (&key)[store::kKeyBytes + 1],
             uint16_t& club) const {
    if (at == sizes[kKeys]) return false;
    const auto* row = data(kKeys) + at;
    club = binary::readU16(row);
    std::memcpy(key + row[2], row + 4, row[3]);
    key[row[2] + row[3]] = 0;
    at += 4 + row[3];
    return true;
  }
  static uint8_t prefix(std::string_view a, std::string_view b) {
    uint8_t n = 0;
    while (n < a.size() && n < b.size() && a[n] == b[n]) ++n;
    return n;
  }
  bool addKey(std::string_view key, uint16_t club) {
    duplicate = false;
    std::size_t at = 0, position = 0, old = 0;
    char previous[store::kKeyBytes + 1]{}, next[store::kKeyBytes + 1]{};
    uint16_t next_club = 0;
    while (at < sizes[kKeys]) {
      position = at;
      if (!keyAt(at, next, next_club)) return false;
      if (key == next) {
        duplicate = true;
        return false;
      }
      if (key < next) {
        old = at - position;
        break;
      }
      std::memcpy(previous, next, sizeof(previous));
      position = at;
    }
    uint8_t rows[2 * (4 + store::kKeyBytes)];
    const auto shared = prefix(previous, key);
    binary::writeU16(rows, club);
    rows[2] = shared;
    rows[3] = uint8_t(key.size() - shared);
    std::memcpy(rows + 4, key.data() + shared, key.size() - shared);
    auto length = 4 + key.size() - shared;
    if (old) {
      const auto next_size = std::strlen(next);
      const auto common = prefix(key, next);
      binary::writeU16(rows + length, next_club);
      rows[length + 2] = common;
      rows[length + 3] = uint8_t(next_size - common);
      std::memcpy(rows + length + 4, next + common, next_size - common);
      length += 4 + next_size - common;
    }
    return replace(kKeys, position, old, rows, length) && fits();
  }
  bool addBinding(unsigned provider, uint32_t id, uint16_t club) {
    duplicate = false;
    const unsigned part = kBindings + provider;
    std::size_t at = 0, position = 0, old = 0;
    uint32_t previous = 0, next = 0, next_club = 0;
    while (at < sizes[part]) {
      position = at;
      next = previous + unvar(data(part), at);
      next_club = unvar(data(part), at);
      if (id == next) {
        duplicate = true;
        return false;
      }
      if (id < next) {
        old = at - position;
        break;
      }
      previous = next;
      position = at;
    }
    uint8_t rows[20];
    auto length = var(id - previous, rows);
    length += var(club, rows + length);
    if (old) {
      length += var(next - id, rows + length);
      length += var(next_club, rows + length);
    }
    if (!replace(part, position, old, rows, length)) return false;
    ++counts.bindings[provider];
    return fits();
  }
  bool addException(uint8_t provider, uint32_t id, uint16_t name) {
    duplicate = false;
    const auto identity = (uint64_t(provider) << 32) | id;
    for (std::size_t i = 0; i < counts.exceptions; ++i) {
      const auto* row = data(kExceptions) + i * 7;
      const auto other = (uint64_t(row[0]) << 32) | binary::readU32(row + 1);
      if (identity == other) {
        duplicate = true;
        return false;
      }
    }
    uint8_t row[7];
    row[0] = provider;
    binary::writeU32(row + 1, id);
    binary::writeU16(row + 5, name);
    if (!append(kExceptions, row, sizeof(row))) return false;
    ++counts.exceptions;
    return fits();
  }
};
// A streamed unique value occupies only its eventual pool space. If it matches
// an existing value, keep that reference while validating all trailing input.
// Candidate-prefix comparisons need neither a second long buffer nor hashes.
struct Intern {
  Draft& draft;
  std::size_t length = 0;
  uint16_t match = 0;
  bool invalid = false, overflow = false;
  bool byte(uint8_t value) {
    if (!value) {
      invalid = true;
      return false;
    }
    const auto old = match;
    if (!match || length >= draft.text(match).size() ||
        static_cast<uint8_t>(draft.text(match)[length]) != value) {
      match = 0;
      const auto* prefix = old ? draft.text(old).data()
                               : reinterpret_cast<const char*>(
                                     draft.bytes.data() + draft.used + 2);
      for (std::size_t at = 0; at < draft.sizes[Draft::kPool];) {
        const auto text = draft.text(uint16_t(at + 1));
        if (text.size() > length &&
            static_cast<uint8_t>(text[length]) == value &&
            std::memcmp(text.data(), prefix, length) == 0) {
          match = uint16_t(at + 1);
          break;
        }
        at += text.size() + 2;
      }
    }
    const auto available =
        std::min(draft.bytes.size() - draft.used,
                 cfg::kTeamNamesFileBytes - draft.sizes[Draft::kPool]);
    if (length + 2 < available)
      draft.bytes[draft.used + 2 + length] = value;
    else if (!match) {
      overflow = true;
      return false;
    }
    ++length;
    return true;
  }
  static bool write(std::string_view bytes, void* raw) {
    auto& self = *static_cast<Intern*>(raw);
    for (const unsigned char value : bytes)
      if (!self.byte(value)) return false;
    return true;
  }
  bool finish(uint16_t& reference) {
    if (!length) {
      for (std::size_t at = 0; at < draft.sizes[Draft::kPool];) {
        const auto text = draft.text(uint16_t(at + 1));
        if (text.empty()) {
          reference = uint16_t(at + 1);
          return true;
        }
        at += text.size() + 2;
      }
    }
    if (match && draft.text(match).size() == length) {
      reference = match;
      return true;
    }
    if (length + 2 > draft.bytes.size() - draft.used ||
        length + 2 > cfg::kTeamNamesFileBytes - draft.sizes[Draft::kPool])
      return false;
    reference = uint16_t(draft.sizes[Draft::kPool] + 1);
    binary::writeU16(draft.bytes.data() + draft.used, uint16_t(length));
    draft.used += length + 2;
    draft.sizes[Draft::kPool] += length + 2;
    return draft.fits();
  }
};
struct Decoder {
  ::json::Reader& reader;
  Draft& draft;
  settings::Violation& why;
  ::json::Token token;
  char buffer[store::kKeyBytes + 1];
  Decode result = Decode::kOk;
  uint32_t version = 0;
  bool inline_values = false, referenced_values = false, has_texts = false;
  bool reject(const FieldPath& path, Reason reason) {
    why.set(path, reason);
    result = Decode::kViolation;
    return false;
  }
  bool next() {
    if (reader.next(token, buffer, sizeof(buffer))) return true;
    result = Decode::kSyntax;
    return false;
  }
  template <std::size_t N, class F>
  bool object(const FieldPath& path, const char* const (&keys)[N],
              uint64_t required, F field) {
    if (token.kind != Kind::kObject) return reject(path, Reason::kType);
    ::json::Fields seen(keys, N);
    while (next()) {
      if (token.kind == Kind::kEndObject)
        return seen.complete(required) || reject(path, Reason::kRequired);
      const int id = seen.accept(token);
      if (id < 0)
        return reject(path,
                      id == -2 ? Reason::kDuplicate : Reason::kUnknownKey);
      if (!field(id, path.with(keys[id]))) return false;
    }
    return false;
  }
  bool number(uint32_t& value, const FieldPath& path, bool positive = true) {
    if (!next()) return false;
    if (token.kind != Kind::kNumber) return reject(path, Reason::kType);
    value = token.truncated ? 0 : values::positiveUint32(token.text);
    if (!positive && token.text == "0" && !token.truncated) return true;
    return value != 0 || reject(path, Reason::kRange);
  }
  bool reference(uint16_t& value, const FieldPath& path) {
    const auto id = token.truncated ? 0 : values::positiveUint32(token.text);
    if (!id || id > cfg::kTeamNamesFileBytes)
      return reject(path, Reason::kRange);
    value = uint16_t(0x8000u | id);
    referenced_values = true;
    return true;
  }
  bool string(uint16_t& value, std::size_t limit, const FieldPath& path,
              bool allow_end = false, bool dictionary = false) {
    char number[10];
    if (limit == SIZE_MAX) {
      Intern intern{draft};
      if (!reader.next(token, number, sizeof(number),
                       {&intern, Intern::write})) {
        if (intern.invalid || intern.overflow)
          return reject(path,
                        intern.invalid ? Reason::kInvalid : Reason::kTooMany);
        result = Decode::kSyntax;
        return false;
      }
      if (allow_end && token.kind == Kind::kEndArray) return true;
      if (!dictionary && token.kind == Kind::kNumber)
        return reference(value, path);
      if (token.kind != Kind::kString) return reject(path, Reason::kType);
      inline_values |= !dictionary;
      return intern.finish(value) || reject(path, Reason::kTooMany);
    }
    struct Prefix {
      std::array<char, cfg::kTeamNameBytes> bytes;
      values::Utf8 utf8;
      std::size_t used = 0, complete = 0, limit;
      bool nul = false;
      static bool write(std::string_view text, void* raw) {
        auto& self = *static_cast<Prefix*>(raw);
        for (const unsigned char byte : text) {
          if (!byte) {
            self.nul = true;
            return false;
          }
          if (!self.utf8.accept(byte)) return false;
          if (self.used < self.limit) {
            self.bytes[self.used++] = static_cast<char>(byte);
            if (self.utf8.complete()) self.complete = self.used;
          }
        }
        return true;
      }
    } prefix{{}, {}, 0, 0, limit};
    if (!reader.next(token, number, sizeof(number), {&prefix, Prefix::write})) {
      if (prefix.nul) return reject(path, Reason::kInvalid);
      result = Decode::kSyntax;
      return false;
    }
    if (token.kind == Kind::kNumber) return reference(value, path);
    if (token.kind != Kind::kString) return reject(path, Reason::kType);
    inline_values = true;
    Intern intern{draft};
    return (Intern::write({prefix.bytes.data(), prefix.complete}, &intern) &&
            intern.finish(value)) ||
           reject(path, Reason::kTooMany);
  }
  bool aliases(uint16_t club, const FieldPath& path) {
    if (!next()) return false;
    if (token.kind != Kind::kArray) return reject(path, Reason::kType);
    std::size_t index = 0;
    while (true) {
      uint16_t ref = 0;
      if (!string(ref, SIZE_MAX, path.withIndex(index), true)) return false;
      if (token.kind == Kind::kEndArray) return true;
      uint8_t row[4];
      binary::writeU16(row, club);
      binary::writeU16(row + 2, ref);
      if (!draft.append(Draft::kAliases, row, sizeof(row)))
        return reject(path.withIndex(index), Reason::kTooMany);
      ++draft.counts.aliases;
      if (!draft.fits()) return reject(path.withIndex(index), Reason::kTooMany);
      ++index;
    }
  }
  bool texts(const FieldPath& path) {
    has_texts = true;
    if (!next()) return false;
    if (token.kind != Kind::kObject) return reject(path, Reason::kType);
    while (next()) {
      if (token.kind == Kind::kEndObject) return true;
      const auto id = token.kind == Kind::kKey && !token.truncated
                          ? values::positiveUint32(token.text)
                          : 0;
      if (!id || id > cfg::kTeamNamesFileBytes || token.text.front() == '0')
        return reject(path, Reason::kInvalid);
      uint16_t value = 0;
      const auto at = path.withIndex(id);
      if (!string(value, SIZE_MAX, at, false, true)) return false;
      if (!draft.addText(uint16_t(id), value))
        return reject(at,
                      draft.duplicate ? Reason::kDuplicate : Reason::kTooMany);
    }
    return false;
  }
  bool resolve(uint8_t* field, std::size_t limit, const FieldPath& path) {
    const auto value = binary::readU16(field);
    if (!(value & 0x8000)) return reject(path, Reason::kType);
    auto* entry = draft.findText(value & 0x7fff);
    if (!entry) return reject(path, Reason::kRequired);
    const auto reference = uint16_t(binary::readU16(entry) & 0x7fff);
    if (draft.text(reference).size() > limit)
      return reject(path, Reason::kTooLong);
    binary::writeU16(entry, uint16_t(reference | 0x8000));
    binary::writeU16(field, reference);
    return true;
  }
  bool resolveTexts(const FieldPath& path) {
    const char* fields[] = {"de", "en", "short", "league"};
    const std::size_t limits[] = {cfg::kTeamNameBytes, cfg::kTeamNameBytes,
                                  cfg::kTeamShortBytes, SIZE_MAX};
    for (uint16_t i = 0; i < draft.counts.clubs; ++i) {
      auto* row = draft.data(Draft::kClubs) + i * 9;
      for (unsigned field = 0; field < 4; ++field)
        if (row[8] & (1u << field))
          if (!resolve(row + field * 2, limits[field],
                       path.with("clubs").withIndex(i).with(fields[field])))
            return false;
    }
    uint16_t previous_club = store::kNone, local_alias = 0;
    for (uint16_t i = 0; i < draft.counts.aliases; ++i) {
      auto* row = draft.data(Draft::kAliases) + i * 4;
      const auto club = binary::readU16(row);
      if (club != previous_club) local_alias = 0;
      previous_club = club;
      if (!resolve(row + 2, SIZE_MAX,
                   path.with("clubs").withIndex(club).with("aliases").withIndex(
                       local_alias++)))
        return false;
    }
    for (uint16_t i = 0; i < draft.counts.exceptions; ++i)
      if (!resolve(draft.data(Draft::kExceptions) + i * 7 + 5,
                   cfg::kTeamShortBytes,
                   path.with("exceptions").withIndex(i).with("name")))
        return false;
    for (std::size_t at = 0; at < draft.sizes[Draft::kTexts]; at += 4)
      if (!(binary::readU16(draft.data(Draft::kTexts) + at + 2) & 0x8000))
        return reject(path.with("texts"), Reason::kConflict);
    return true;
  }
  bool ids(uint16_t club, const FieldPath& path) {
    if (!next()) return false;
    const char* keys[cfg::kProviderCount]{};
    // Use the actual provider keys; their stable names are defined centrally.
    for (std::size_t i = 0; i < cfg::kProviderCount; ++i)
      keys[i] = providerName(static_cast<cfg::Provider>(i));
    return object(path, keys, 0, [&](int provider, const FieldPath& at) {
      if (!next()) return false;
      if (token.kind != Kind::kArray) return reject(at, Reason::kType);
      std::size_t index = 0;
      while (next()) {
        if (token.kind == Kind::kEndArray) return true;
        const auto value = token.kind == Kind::kNumber && !token.truncated
                               ? values::positiveUint32(token.text)
                               : 0;
        if (!value)
          return reject(at.withIndex(index), token.kind == Kind::kNumber
                                                 ? Reason::kRange
                                                 : Reason::kType);
        if (!draft.addBinding(unsigned(provider), value, club))
          return reject(at.withIndex(index), draft.duplicate
                                                 ? Reason::kDuplicate
                                                 : Reason::kTooMany);
        ++index;
      }
      return false;
    });
  }
  bool clubs(const FieldPath& path) {
    if (!next()) return false;
    if (token.kind != Kind::kArray) return reject(path, Reason::kType);
    while (next()) {
      if (token.kind == Kind::kEndArray) return true;
      const auto index = draft.counts.clubs;
      const auto at = path.withIndex(index);
      if (!draft.append(Draft::kClubs, nullptr, 9))
        return reject(at, Reason::kTooMany);
      ++draft.counts.clubs;
      if (!draft.fits()) return reject(at, Reason::kTooMany);
      const char* keys[] = {"key",    "de",  "en",     "short",
                            "league", "ids", "aliases"};
      if (!object(at, keys, 1, [&](int field, const FieldPath& key_path) {
            if (field == 0) {
              if (!next()) return false;
              if (token.kind != Kind::kString)
                return reject(key_path, Reason::kType);
              if (token.truncated || token.text.empty() ||
                  token.text.size() > store::kKeyBytes ||
                  !values::isUtf8Text(token.text))
                return reject(key_path, Reason::kInvalid);
              return draft.addKey(token.text, index) ||
                     reject(key_path, draft.duplicate ? Reason::kDuplicate
                                                      : Reason::kTooMany);
            }
            if (field == 5) return ids(index, key_path);
            if (field == 6) return aliases(index, key_path);
            uint16_t reference = 0;
            const auto limit = field <= 2   ? cfg::kTeamNameBytes
                               : field == 3 ? cfg::kTeamShortBytes
                                            : SIZE_MAX;
            if (!string(reference, limit, key_path)) return false;
            auto* row = draft.data(Draft::kClubs) + index * 9;
            binary::writeU16(row + (field - 1) * 2, reference);
            row[8] |= uint8_t(1u << (field - 1));
            return true;
          }))
        return false;
    }
    return false;
  }
  bool exceptions(const FieldPath& path) {
    if (!next()) return false;
    if (token.kind != Kind::kArray) return reject(path, Reason::kType);
    std::size_t index = 0;
    while (next()) {
      if (token.kind == Kind::kEndArray) return true;
      uint32_t provider = 0, id = 0;
      uint16_t name = 0;
      const char* keys[] = {"provider", "id", "name"};
      const auto at = path.withIndex(index++);
      if (!object(at, keys, 7, [&](int field, const FieldPath& key_path) {
            if (field == 0)
              return number(provider, key_path, false) &&
                     (provider < cfg::kProviderCount ||
                      reject(key_path, Reason::kRange));
            if (field == 1) return number(id, key_path);
            return string(name, cfg::kTeamShortBytes, key_path);
          }))
        return false;
      if (!draft.addException(uint8_t(provider), id, name))
        return reject(at,
                      draft.duplicate ? Reason::kDuplicate : Reason::kTooMany);
    }
    return false;
  }
};
bool prepareMap(Draft& draft) {
  // Preserve source order for truthful reference-error paths, then normalize
  // exceptions for their sorted wire contract without another row array.
  auto* exceptions = draft.data(Draft::kExceptions);
  const auto identity = [](const uint8_t* row) {
    return (uint64_t(row[0]) << 32) | binary::readU32(row + 1);
  };
  for (std::size_t i = 1; i < draft.counts.exceptions; ++i) {
    uint8_t row[7];
    std::memcpy(row, exceptions + i * 7, sizeof(row));
    auto at = i;
    while (at && identity(exceptions + (at - 1) * 7) > identity(row)) {
      std::memcpy(exceptions + at * 7, exceptions + (at - 1) * 7, 7);
      --at;
    }
    std::memcpy(exceptions + at * 7, row, sizeof(row));
  }
  if (!draft.append(Draft::kMap, nullptr, draft.counts.clubs * 2)) return false;
  char key[store::kKeyBytes + 1]{};
  std::size_t at = 0;
  uint16_t old = 0, sorted = 0;
  while (draft.keyAt(at, key, old))
    binary::writeU16(draft.data(Draft::kMap) + old * 2, sorted++);
  return sorted == draft.counts.clubs;
}
std::size_t wireSize(const Draft& draft) {
  const auto checkpoints = [](std::size_t count) {
    return (count + store::kCheckpoint - 1) / store::kCheckpoint;
  };
  auto size = store::kHeaderBytes + draft.sizes[Draft::kClubs] +
              draft.sizes[Draft::kAliases] + draft.sizes[Draft::kExceptions] +
              draft.sizes[Draft::kPool] + checkpoints(draft.counts.clubs) * 2;
  char key[store::kKeyBytes + 1]{};
  std::size_t at = 0;
  uint16_t old = 0, sorted = 0;
  while (at < draft.sizes[Draft::kKeys]) {
    const auto shared = draft.data(Draft::kKeys)[at + 2];
    const auto suffix = draft.data(Draft::kKeys)[at + 3];
    if (!draft.keyAt(at, key, old)) return SIZE_MAX;
    size += 2 + suffix + (sorted++ % store::kCheckpoint == 0 ? shared : 0);
  }
  for (unsigned provider = 0; provider < cfg::kProviderCount; ++provider) {
    size += checkpoints(draft.counts.bindings[provider]) * 6;
    const auto part = Draft::kBindings + provider;
    uint32_t id = 0, previous = 0;
    at = 0;
    unsigned index = 0;
    uint8_t scratch[10];
    while (at < draft.sizes[part]) {
      id += unvar(draft.data(part), at);
      const auto input = uint16_t(unvar(draft.data(part), at));
      const auto position =
          binary::readU16(draft.data(Draft::kMap) + input * 2);
      size += var(index++ % store::kCheckpoint ? id - previous : id, scratch);
      size += var(position, scratch);
      previous = id;
    }
  }
  return size;
}
bool emit(Draft& draft, files::Candidate& candidate) {
  store::Writer writer;
  if (!writer.begin(candidate.io(), draft.counts,
                    candidate.expectedReference().generation + 1, 0))
    return false;
  char key[store::kKeyBytes + 1]{};
  std::size_t at = 0;
  uint16_t old = 0, sorted = 0;
  while (draft.keyAt(at, key, old)) {
    if (!writer.key(key)) return false;
  }
  const auto map = [&](uint16_t input) {
    return binary::readU16(draft.data(Draft::kMap) + input * 2);
  };
  for (unsigned provider = 0; provider < cfg::kProviderCount; ++provider) {
    const auto part = Draft::kBindings + provider;
    uint32_t id = 0;
    at = 0;
    while (at < draft.sizes[part]) {
      id += unvar(draft.data(part), at);
      const auto club = uint16_t(unvar(draft.data(part), at));
      if (!writer.binding(static_cast<cfg::Provider>(provider), id, map(club)))
        return false;
    }
  }
  at = 0;
  sorted = 0;
  key[0] = 0;
  while (draft.keyAt(at, key, old)) {
    const auto* row = draft.data(Draft::kClubs) + old * 9;
    store::Fields fields;
    store::Field* values[] = {&fields.de, &fields.en, &fields.short_name,
                              &fields.league};
    for (unsigned field = 0; field < 4; ++field)
      if (row[8] & (1u << field))
        *values[field] = {true, draft.text(binary::readU16(row + field * 2))};
    if (!writer.club(sorted++, fields)) return false;
  }
  for (uint16_t i = 0; i < draft.counts.aliases; ++i) {
    const auto* row = draft.data(Draft::kAliases) + i * 4;
    if (!writer.alias(map(binary::readU16(row)),
                      draft.text(binary::readU16(row + 2))))
      return false;
  }
  for (uint16_t i = 0; i < draft.counts.exceptions; ++i) {
    const auto* row = draft.data(Draft::kExceptions) + i * 7;
    if (!writer.exception(static_cast<cfg::Provider>(row[0]),
                          binary::readU32(row + 1),
                          draft.text(binary::readU16(row + 5))))
      return false;
  }
  const auto size = writer.finish();
  return size && candidate.validate(size) == files::Status::kOk;
}
}  // namespace
Decode readAdoptedJson(::json::Reader& reader, const ::json::Token& object,
                       files::Candidate& candidate, settings::Violation& why) {
  auto scratch = std::unique_ptr<Draft>(new (std::nothrow) Draft);
  if (!scratch) return Decode::kNoMemory;
  Decoder decoder{reader, *scratch, why, object, {}};
  const auto root = FieldPath().key("adopted_names");
  const char* keys[] = {"format", "version", "clubs", "exceptions", "texts"};
  if (!decoder.object(root, keys, 7, [&](int field, const FieldPath& path) {
        if (field == 2) return decoder.clubs(path);
        if (field == 3) return decoder.exceptions(path);
        if (field == 4) return decoder.texts(path);
        if (field == 1) {
          return decoder.number(decoder.version, path) &&
                 (decoder.version == cfg::kTeamNamesJsonVersion ||
                  decoder.version == cfg::kAdoptedNamesJsonVersion ||
                  decoder.reject(path, Reason::kRange));
        }
        if (!decoder.next()) return false;
        return (decoder.token.kind == Kind::kString &&
                !decoder.token.truncated &&
                decoder.token.text == cfg::kTeamNamesJsonFormat) ||
               decoder.reject(path, Reason::kInvalid);
      }))
    return decoder.result;
  if (decoder.version == cfg::kTeamNamesJsonVersion) {
    if (decoder.has_texts || decoder.referenced_values) {
      decoder.reject(root, Reason::kType);
      return decoder.result;
    }
  } else if (!decoder.has_texts || decoder.inline_values) {
    decoder.reject(root, Reason::kType);
    return decoder.result;
  } else if (!decoder.resolveTexts(root))
    return decoder.result;
  if (!prepareMap(*scratch) || wireSize(*scratch) > cfg::kTeamNamesFileBytes) {
    decoder.reject(root, Reason::kTooMany);
    return decoder.result;
  }
  if (!emit(*scratch, candidate)) {
    if (candidate.failure() == files::Status::kPhysicalSpace)
      return Decode::kStorageFull;
    if (candidate.failure() == files::Status::kNoMemory)
      return Decode::kNoMemory;
    return Decode::kIo;
  }
  return Decode::kOk;
}
}  // namespace football::names
