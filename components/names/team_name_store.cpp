#include "team_name_store.h"

#include <algorithm>
#include <cstring>

#include "binary_codec.h"
#include "value_utils.h"

namespace football::names::store {
namespace {
// Wire order: 96-byte header, 9-byte club records, 4-byte alias references,
// 7-byte ID exceptions, key checkpoints (2 bytes per 16 keys), binding
// checkpoints (first ID4 + offset2 per 16 bindings), prefix keys, four provider
// binding streams, then deduplicated text. All integers are little endian.
// Keys store prefix/suffix byte lengths followed by the complete suffix.
// Binding streams store unsigned ID deltas and file-local club indices;
// each checkpoint starts with an absolute ID. Text is length2 + UTF-8 bytes.
// The CRC covers the complete file with the CRC field itself treated as zero.
constexpr uint32_t kMagic = 0x32534e46;  // FNS2
constexpr uint16_t kVersion = 1;
static_assert(cfg::kProviderCount == 4,
              "The wire header has four provider sections");
static_assert(cfg::kTeamNamesFileBytes <= UINT16_MAX);
constexpr std::size_t kTotal = 8, kClubs = 10, kAliases = 12, kExceptions = 14;
constexpr std::size_t kRecords = 24, kAliasTable = 26, kExceptionTable = 28;
constexpr std::size_t kKeyIndex = 30, kBindingIndex = 32, kKeys = 40;
constexpr std::size_t kBindings = 42, kPool = 52, kBindingCounts = 54,
                      kCrc = 92;
std::size_t blocks(std::size_t n) {
  return (n + kCheckpoint - 1) / kCheckpoint;
}
bool readVar(const IO& io, std::size_t& at, std::size_t end, uint32_t& value) {
  value = 0;
  for (unsigned shift = 0; shift < 35; shift += 7) {
    uint8_t c;
    if (at >= end || !io.read(at++, &c, 1, io.context) ||
        (shift == 28 && (c & 0xf0)))
      return false;
    value |= uint32_t(c & 0x7f) << shift;
    if (!(c & 0x80))
      return shift == 0 || c != 0;  // canonical unsigned representation
  }
  return false;
}
std::size_t var(uint32_t value, uint8_t* out) {
  std::size_t n = 0;
  do {
    out[n++] = uint8_t(value & 0x7f) | (value > 127 ? 0x80 : 0);
    value >>= 7;
  } while (value);
  return n;
}
bool readKey(const IO& io, std::size_t& at, std::size_t end, char* out,
             bool restart) {
  uint8_t sizes[2];
  if (at + 2 > end || !io.read(at, sizes, 2, io.context)) return false;
  at += 2;
  const auto old = std::strlen(out);
  if ((restart && sizes[0] != 0) || sizes[0] > old ||
      sizes[0] + sizes[1] > kKeyBytes || at + sizes[1] > end)
    return false;
  if (!io.read(at, out + sizes[0], sizes[1], io.context)) return false;
  at += sizes[1];
  out[sizes[0] + sizes[1]] = 0;
  return out[0] &&
         values::isUtf8Text(std::string_view(out, sizes[0] + sizes[1]));
}
}  // namespace

bool adopt(std::string_view input, std::size_t limit,
           std::string_view& prefix) {
  return values::utf8TextPrefix(input, limit, prefix);
}

Counts Reader::counts() const {
  Counts c;
  c.clubs = binary::readU16(&header_[kClubs]);
  c.aliases = binary::readU16(&header_[kAliases]);
  c.exceptions = binary::readU16(&header_[kExceptions]);
  for (std::size_t p = 0; p < cfg::kProviderCount; ++p)
    c.bindings[p] = binary::readU16(&header_[kBindingCounts + 2 * p]);
  return c;
}
uint32_t Reader::generation() const { return binary::readU32(&header_[16]); }
uint32_t Reader::digest() const { return binary::readU32(&header_[kCrc]); }
uint32_t Reader::sourceFingerprint() const {
  return binary::readU32(&header_[20]);
}
std::size_t Reader::size() const { return binary::readU16(&header_[kTotal]); }
uint16_t Reader::firstText() const {
  const auto at = binary::readU16(&header_[kPool]);
  return at < size() ? at : 0;
}
bool Reader::nextText(uint16_t& reference) const {
  uint8_t bytes[2];
  if (!reference || !get(reference, bytes, sizeof(bytes))) return false;
  const auto next = std::size_t(reference) + 2 + binary::readU16(bytes);
  if (next > size()) return false;
  reference = next < size() ? uint16_t(next) : 0;
  return true;
}
bool Reader::get(std::size_t at, void* out, std::size_t n) const {
  return io_.read && at <= size() && n <= size() - at &&
         io_.read(at, out, n, io_.context);
}

bool Reader::reuse(IO io, const Reader& checked, std::size_t exact_size) {
  uint8_t receipt[kHeaderBytes];
  if (!io.read || binary::readU32(checked.header_.data()) != kMagic ||
      exact_size != checked.size() ||
      !io.read(0, receipt, sizeof(receipt), io.context) ||
      std::memcmp(receipt, checked.header_.data(), sizeof(receipt)) != 0) {
    *this = Reader{};
    return false;
  }
  header_ = checked.header_;
  io_ = io;
  return true;
}

bool Reader::key(uint16_t index, char (&out)[kKeyBytes + 1]) const {
  if (index >= binary::readU16(&header_[kClubs])) return false;
  uint8_t offset[2];
  if (!get(binary::readU16(&header_[kKeyIndex]) + (index / kCheckpoint) * 2,
           offset, 2))
    return false;
  std::size_t at = binary::readU16(offset);
  out[0] = 0;
  for (std::size_t n = 0; n <= index % kCheckpoint; ++n)
    if (!readKey(io_, at, binary::readU16(&header_[kBindings]), out, n == 0))
      return false;
  return true;
}
bool Reader::club(uint16_t index, Club& out) const {
  uint8_t b[9];
  if (index >= binary::readU16(&header_[kClubs]) ||
      !get(binary::readU16(&header_[kRecords]) + index * 9, b, sizeof(b)))
    return false;
  out = {binary::readU16(b), binary::readU16(b + 2), binary::readU16(b + 4),
         binary::readU16(b + 6), b[8]};
  return true;
}
bool Reader::alias(uint16_t index, Alias& out) const {
  uint8_t b[4];
  if (index >= binary::readU16(&header_[kAliases]) ||
      !get(binary::readU16(&header_[kAliasTable]) + index * 4, b, sizeof(b)))
    return false;
  out = {binary::readU16(b), binary::readU16(b + 2)};
  return true;
}
bool Reader::exception(uint16_t index, Exception& out) const {
  uint8_t b[7];
  if (index >= binary::readU16(&header_[kExceptions]) ||
      !get(binary::readU16(&header_[kExceptionTable]) + index * 7, b,
           sizeof(b)))
    return false;
  out = {static_cast<cfg::Provider>(b[0]), binary::readU32(b + 1),
         binary::readU16(b + 5)};
  return true;
}
bool Reader::binding(cfg::Provider provider, uint16_t index,
                     Binding& out) const {
  const auto p = static_cast<std::size_t>(provider);
  if (p >= cfg::kProviderCount ||
      index >= binary::readU16(&header_[kBindingCounts + 2 * p]))
    return false;
  uint8_t checkpoint[6];
  if (!get(binary::readU16(&header_[kBindingIndex + p * 2]) +
               (index / kCheckpoint) * 6,
           checkpoint, sizeof(checkpoint)))
    return false;
  std::size_t at = binary::readU16(checkpoint + 4);
  uint32_t id = 0, delta = 0, club = 0;
  const auto end = binary::readU16(&header_[kBindings + (p + 1) * 2]);
  for (std::size_t n = 0; n <= index % kCheckpoint; ++n) {
    if (!readVar(io_, at, end, delta) || !delta || delta > UINT32_MAX - id ||
        !readVar(io_, at, end, club) ||
        club >= binary::readU16(&header_[kClubs]))
      return false;
    id += delta;
    if (!n && id != binary::readU32(checkpoint)) return false;
  }
  out = {id, uint16_t(club)};
  return true;
}
Lookup Reader::findKey(std::string_view wanted, uint16_t& found) const {
  const auto count = binary::readU16(&header_[kClubs]);
  if (!count) return Lookup::kMissing;
  if (wanted.empty() || wanted.size() > kKeyBytes ||
      !values::isUtf8Text(wanted))
    return Lookup::kMissing;
  char value[kKeyBytes + 1]{};
  std::size_t low = 0, high = blocks(count);
  while (low < high) {
    const auto mid = (low + high) / 2;
    if (!key(uint16_t(mid * kCheckpoint), value)) return Lookup::kError;
    if (std::string_view(value) <= wanted)
      low = mid + 1;
    else
      high = mid;
  }
  if (!low) return Lookup::kMissing;
  uint8_t offset[2];
  if (!get(binary::readU16(&header_[kKeyIndex]) + (low - 1) * 2, offset, 2))
    return Lookup::kError;
  std::size_t at = binary::readU16(offset);
  value[0] = 0;
  const auto start = (low - 1) * kCheckpoint,
             end = std::min<std::size_t>(start + kCheckpoint, count);
  for (auto i = start; i < end; ++i) {
    if (!readKey(io_, at, binary::readU16(&header_[kBindings]), value,
                 i == start))
      return Lookup::kError;
    const auto compare = wanted.compare(value);
    if (!compare) {
      found = uint16_t(i);
      return Lookup::kFound;
    }
    if (compare < 0) break;
  }
  return Lookup::kMissing;
}
Lookup Reader::findId(cfg::Provider provider, uint32_t wanted,
                      uint16_t& found) const {
  const auto p = static_cast<std::size_t>(provider);
  if (p >= cfg::kProviderCount || !wanted) return Lookup::kMissing;
  const auto count = binary::readU16(&header_[kBindingCounts + 2 * p]);
  std::size_t low = 0, high = blocks(count);
  uint8_t cp[6];
  while (low < high) {
    const auto mid = (low + high) / 2;
    if (!get(binary::readU16(&header_[kBindingIndex + p * 2]) + mid * 6, cp, 6))
      return Lookup::kError;
    if (binary::readU32(cp) <= wanted)
      low = mid + 1;
    else
      high = mid;
  }
  if (!low) return Lookup::kMissing;
  if (!get(binary::readU16(&header_[kBindingIndex + p * 2]) + (low - 1) * 6, cp,
           6))
    return Lookup::kError;
  std::size_t at = binary::readU16(cp + 4);
  uint32_t id = 0, delta = 0, club = 0;
  const auto end = binary::readU16(&header_[kBindings + (p + 1) * 2]);
  const auto start = (low - 1) * kCheckpoint,
             stop = std::min<std::size_t>(start + kCheckpoint, count);
  for (auto i = start; i < stop; ++i) {
    if (!readVar(io_, at, end, delta) || !delta || delta > UINT32_MAX - id ||
        !readVar(io_, at, end, club) ||
        club >= binary::readU16(&header_[kClubs]))
      return Lookup::kError;
    id += delta;
    if (id == wanted) {
      found = uint16_t(club);
      return Lookup::kFound;
    }
    if (id > wanted) break;
  }
  return Lookup::kMissing;
}
Lookup Reader::findException(cfg::Provider provider, uint32_t wanted,
                             uint16_t& text_ref) const {
  const auto p = static_cast<uint8_t>(provider);
  std::size_t low = 0, high = binary::readU16(&header_[kExceptions]);
  while (low < high) {
    const auto mid = (low + high) / 2;
    Exception e;
    if (!exception(uint16_t(mid), e)) return Lookup::kError;
    const auto ep = static_cast<uint8_t>(e.provider);
    if (ep < p || (ep == p && e.id < wanted))
      low = mid + 1;
    else
      high = mid;
  }
  if (low == binary::readU16(&header_[kExceptions])) return Lookup::kMissing;
  Exception e;
  if (!exception(uint16_t(low), e)) return Lookup::kError;
  if (e.provider != provider || e.id != wanted) return Lookup::kMissing;
  text_ref = e.text;
  return Lookup::kFound;
}
bool Reader::text(uint16_t ref, char* out, std::size_t capacity,
                  std::size_t& length) const {
  uint8_t b[2];
  if (!out || ref < binary::readU16(&header_[kPool]) || !get(ref, b, 2))
    return false;
  length = binary::readU16(b);
  if (length >= capacity || !get(ref + 2, out, length)) return false;
  out[length] = 0;
  return true;
}
bool Reader::streamText(uint16_t ref, Write write, void* context) const {
  uint8_t b[64];
  if (!write || ref < binary::readU16(&header_[kPool]) || !get(ref, b, 2))
    return false;
  const auto length = binary::readU16(b);
  std::size_t at = ref + 2;
  if (at + length > size()) return false;
  for (std::size_t left = length; left;) {
    const auto n = std::min(left, sizeof(b));
    if (!get(at, b, n) || !write(b, n, context)) return false;
    at += n;
    left -= n;
  }
  return true;
}
bool Reader::poolReference(uint16_t ref, std::size_t limit) const {
  if (!ref) return true;
  std::size_t at = binary::readU16(&header_[kPool]);
  uint8_t b[2];
  while (at < size()) {
    if (!get(at, b, 2)) return false;
    const auto length = binary::readU16(b);
    if (at + 2 + length > size()) return false;
    if (at == ref) return length <= limit;
    if (at > ref) return false;
    at += 2 + length;
  }
  return false;
}

bool Reader::open(IO io, std::size_t exact_size) {
  io_ = io;
  header_ = {};
  const auto fail = [&] {
    header_ = {};
    return false;
  };
  if (!io.read || exact_size < kHeaderBytes ||
      exact_size > cfg::kTeamNamesFileBytes ||
      !io.read(0, header_.data(), header_.size(), io.context))
    return fail();
  if (binary::readU32(header_.data()) != kMagic ||
      binary::readU16(&header_[4]) != kVersion ||
      binary::readU16(&header_[6]) != kHeaderBytes || size() != exact_size)
    return fail();
  for (std::size_t i = 62; i < kCrc; ++i)
    if (header_[i]) return fail();
  uint8_t scratch[64];
  uint32_t crc = 0;
  for (std::size_t at = 0; at < exact_size;) {
    const auto n = std::min(sizeof(scratch), exact_size - at);
    if (!get(at, scratch, n)) return fail();
    for (std::size_t j = 0; j < n; ++j)
      if (at + j >= kCrc && at + j < kCrc + 4) scratch[j] = 0;
    crc = binary::crc32({scratch, n}, crc);
    at += n;
  }
  if (crc != binary::readU32(&header_[kCrc])) return fail();
  const auto c = counts();
  std::size_t at = kHeaderBytes;
  if (binary::readU16(&header_[kRecords]) != at) return fail();
  at += c.clubs * 9;
  if (binary::readU16(&header_[kAliasTable]) != at) return fail();
  at += c.aliases * 4;
  if (binary::readU16(&header_[kExceptionTable]) != at) return fail();
  at += c.exceptions * 7;
  if (binary::readU16(&header_[kKeyIndex]) != at) return fail();
  at += blocks(c.clubs) * 2;
  for (std::size_t p = 0; p < cfg::kProviderCount; ++p) {
    if (binary::readU16(&header_[kBindingIndex + 2 * p]) != at) return fail();
    at += blocks(c.bindings[p]) * 6;
  }
  if (binary::readU16(&header_[kKeys]) != at || at > size() ||
      binary::readU16(&header_[kBindings]) < at ||
      binary::readU16(&header_[kBindings]) > size())
    return fail();
  char previous[kKeyBytes + 1]{}, current[kKeyBytes + 1]{};
  for (uint16_t i = 0; i < c.clubs; ++i) {
    if (i % kCheckpoint == 0) {
      uint8_t b[2];
      if (!get(binary::readU16(&header_[kKeyIndex]) + (i / kCheckpoint) * 2, b,
               2) ||
          binary::readU16(b) != at)
        return fail();
    }
    if (!readKey(io_, at, binary::readU16(&header_[kBindings]), current,
                 i % kCheckpoint == 0) ||
        (i && std::strcmp(previous, current) >= 0))
      return fail();
    std::memcpy(previous, current, sizeof(previous));
  }
  if (at != binary::readU16(&header_[kBindings])) return fail();
  for (std::size_t p = 0; p < cfg::kProviderCount; ++p) {
    if (binary::readU16(&header_[kBindings + 2 * p]) != at) return fail();
    uint32_t previous_id = 0, id = 0;
    const auto end = binary::readU16(&header_[kBindings + (p + 1) * 2]);
    if (end > size() || end < at) return fail();
    for (uint16_t i = 0; i < c.bindings[p]; ++i) {
      if (i % kCheckpoint == 0) {
        uint8_t b[6];
        if (!get(binary::readU16(&header_[kBindingIndex + 2 * p]) +
                     (i / kCheckpoint) * 6,
                 b, 6) ||
            binary::readU16(b + 4) != at)
          return fail();
        id = 0;
        uint32_t value;
        auto cursor = at;
        if (!readVar(io_, cursor, end, value) || value != binary::readU32(b))
          return fail();
      }
      uint32_t delta, club_ref;
      if (!readVar(io_, at, end, delta) || !delta || delta > UINT32_MAX - id ||
          !readVar(io_, at, end, club_ref) || club_ref >= c.clubs)
        return fail();
      id += delta;
      if (id <= previous_id) return fail();
      previous_id = id;
    }
    if (at != end) return fail();
  }
  if (at != binary::readU16(&header_[kPool])) return fail();
  while (at < size()) {
    uint8_t b[2];
    if (!get(at, b, 2)) return fail();
    const auto length = binary::readU16(b);
    at += 2;
    if (at + length > size()) return fail();
    values::Utf8 state;
    for (std::size_t left = length; left;) {
      const auto n = std::min(left, sizeof(scratch));
      if (!get(at, scratch, n)) return fail();
      for (std::size_t j = 0; j < n; ++j)
        if (scratch[j] == 0 || !state.accept(scratch[j])) return fail();
      at += n;
      left -= n;
    }
    if (!state.complete()) return fail();
  }
  if (at != size()) return fail();
  for (uint16_t i = 0; i < c.clubs; ++i) {
    Club row;
    if (!club(i, row) || (row.present & 0xf0)) return fail();
    const uint16_t refs[] = {row.de, row.en, row.short_name, row.league};
    const std::size_t limits[] = {cfg::kTeamNameBytes, cfg::kTeamNameBytes,
                                  cfg::kTeamShortBytes, SIZE_MAX};
    for (unsigned n = 0; n < 4; ++n)
      if (((row.present >> n) & 1) != (refs[n] != 0) ||
          !poolReference(refs[n], limits[n]))
        return fail();
  }
  for (uint16_t i = 0; i < c.aliases; ++i) {
    Alias row;
    if (!alias(i, row) || row.club >= c.clubs || !row.text ||
        !poolReference(row.text))
      return fail();
  }
  Exception previous_exception;
  for (uint16_t i = 0; i < c.exceptions; ++i) {
    Exception row;
    if (!exception(i, row) ||
        static_cast<std::size_t>(row.provider) >= cfg::kProviderCount ||
        !row.id || !row.text || !poolReference(row.text, cfg::kTeamShortBytes))
      return fail();
    const auto p = static_cast<uint8_t>(row.provider),
               pp = static_cast<uint8_t>(previous_exception.provider);
    if (i && (p < pp || (p == pp && row.id <= previous_exception.id)))
      return fail();
    previous_exception = row;
  }
  return true;
}

bool Writer::put(std::size_t at, const void* bytes, std::size_t n) {
  if (failed_ || at > cfg::kTeamNamesFileBytes ||
      n > cfg::kTeamNamesFileBytes - at || !io_.write ||
      !io_.write(at, bytes, n, io_.context)) {
    failed_ = true;
    return false;
  }
  return true;
}
bool Writer::append(const void* bytes, std::size_t n) {
  if (!n) return !failed_;
  if (!put(cursor_, bytes, n)) return false;
  cursor_ = uint16_t(cursor_ + n);
  return true;
}
bool Writer::begin(IO io, const Counts& c, uint32_t generation,
                   uint32_t source_fingerprint) {
  *this = Writer{};
  io_ = io;
  failed_ = !io.read || !io.write;
  binary::writeU32(header_.data(), kMagic);
  binary::writeU16(&header_[4], kVersion);
  binary::writeU16(&header_[6], kHeaderBytes);
  binary::writeU16(&header_[kClubs], c.clubs);
  binary::writeU16(&header_[kAliases], c.aliases);
  binary::writeU16(&header_[kExceptions], c.exceptions);
  binary::writeU32(&header_[16], generation);
  binary::writeU32(&header_[20], source_fingerprint);
  std::size_t at = kHeaderBytes;
  binary::writeU16(&header_[kRecords], uint16_t(at));
  at += c.clubs * 9;
  if (at > cfg::kTeamNamesFileBytes) {
    failed_ = true;
    return false;
  }
  binary::writeU16(&header_[kAliasTable], uint16_t(at));
  at += c.aliases * 4;
  binary::writeU16(&header_[kExceptionTable], uint16_t(at));
  at += c.exceptions * 7;
  binary::writeU16(&header_[kKeyIndex], uint16_t(at));
  at += blocks(c.clubs) * 2;
  for (std::size_t p = 0; p < cfg::kProviderCount; ++p) {
    if (at > cfg::kTeamNamesFileBytes) {
      failed_ = true;
      return false;
    }
    binary::writeU16(&header_[kBindingCounts + 2 * p], c.bindings[p]);
    binary::writeU16(&header_[kBindingIndex + 2 * p], uint16_t(at));
    at += blocks(c.bindings[p]) * 6;
  }
  if (at > cfg::kTeamNamesFileBytes) {
    failed_ = true;
    return false;
  }
  binary::writeU16(&header_[kKeys], uint16_t(at));
  cursor_ = uint16_t(at);
  uint8_t zero[32]{};
  for (std::size_t offset = 0; offset < at;) {
    const auto n = std::min(sizeof(zero), at - offset);
    if (!put(offset, zero, n)) return false;
    offset += n;
  }
  return !failed_;
}
bool Writer::key(std::string_view value) {
  if (failed_ || sealed_ || keys_ >= binary::readU16(&header_[kClubs]) ||
      value.empty() || value.size() > kKeyBytes || !values::isUtf8Text(value) ||
      (keys_ && std::string_view(previous_key_) >= value)) {
    failed_ = true;
    return false;
  }
  uint8_t prefix = 0;
  if (keys_ % kCheckpoint == 0) {
    uint8_t at[2];
    binary::writeU16(at, cursor_);
    if (!put(binary::readU16(&header_[kKeyIndex]) + (keys_ / kCheckpoint) * 2,
             at, 2))
      return false;
  } else
    while (prefix < value.size() && previous_key_[prefix] &&
           previous_key_[prefix] == value[prefix])
      ++prefix;
  const uint8_t sizes[] = {prefix, uint8_t(value.size() - prefix)};
  if (!append(sizes, 2) ||
      !append(value.data() + prefix, value.size() - prefix))
    return false;
  std::memcpy(previous_key_, value.data(), value.size());
  previous_key_[value.size()] = 0;
  ++keys_;
  return true;
}
bool Writer::binding(cfg::Provider provider, uint32_t id, uint16_t club_ref) {
  const auto p = static_cast<uint8_t>(provider);
  if (failed_ || sealed_ || keys_ != binary::readU16(&header_[kClubs]) ||
      p >= cfg::kProviderCount || club_ref >= keys_ || !id || p < provider_) {
    failed_ = true;
    return false;
  }
  while (provider_ < p ||
         binding_count_ ==
             binary::readU16(&header_[kBindingCounts + 2 * provider_])) {
    if (binding_count_ !=
            binary::readU16(&header_[kBindingCounts + 2 * provider_]) ||
        provider_ == cfg::kProviderCount - 1) {
      failed_ = true;
      return false;
    }
    if (!binding_count_)
      binary::writeU16(&header_[kBindings + 2 * provider_], cursor_);
    binary::writeU16(&header_[kBindings + 2 * (provider_ + 1)], cursor_);
    ++provider_;
    binding_count_ = 0;
    previous_id_ = 0;
  }
  if (provider_ != p) {
    failed_ = true;
    return false;
  }
  if (!binding_count_)
    binary::writeU16(&header_[kBindings + 2 * provider_], cursor_);
  if (id <= previous_id_) {
    failed_ = true;
    return false;
  }
  if (binding_count_ % kCheckpoint == 0) {
    uint8_t cp[6];
    binary::writeU32(cp, id);
    binary::writeU16(cp + 4, cursor_);
    if (!put(binary::readU16(&header_[kBindingIndex + 2 * p]) +
                 (binding_count_ / kCheckpoint) * 6,
             cp, 6))
      return false;
  }
  uint8_t bytes[10];
  auto n = var(binding_count_ % kCheckpoint ? id - previous_id_ : id, bytes);
  n += var(club_ref, bytes + n);
  if (!append(bytes, n)) return false;
  previous_id_ = id;
  ++binding_count_;
  return true;
}
bool Writer::seal() {
  if (failed_) return false;
  if (sealed_) return true;
  if (keys_ != binary::readU16(&header_[kClubs])) {
    failed_ = true;
    return false;
  }
  while (provider_ < cfg::kProviderCount) {
    if (binding_count_ !=
        binary::readU16(&header_[kBindingCounts + 2 * provider_])) {
      failed_ = true;
      return false;
    }
    if (!binding_count_)
      binary::writeU16(&header_[kBindings + 2 * provider_], cursor_);
    binary::writeU16(&header_[kBindings + 2 * (provider_ + 1)], cursor_);
    ++provider_;
    binding_count_ = 0;
  }
  binary::writeU16(&header_[kPool], cursor_);
  sealed_ = true;
  return true;
}
bool Writer::intern(std::string_view value, uint16_t& ref) {
  if (!seal() || !values::isUtf8Text(value) ||
      value.size() > cfg::kTeamNamesFileBytes - 2) {
    failed_ = true;
    return false;
  }
  std::size_t at = binary::readU16(&header_[kPool]);
  uint8_t scratch[64];
  while (at < cursor_) {
    if (!io_.read(at, scratch, 2, io_.context)) {
      failed_ = true;
      return false;
    }
    const auto length = binary::readU16(scratch);
    if (at + 2 + length > cursor_) {
      failed_ = true;
      return false;
    }
    bool same = length == value.size();
    for (std::size_t offset = 0; same && offset < length;) {
      const auto n = std::min(sizeof(scratch), length - offset);
      if (!io_.read(at + 2 + offset, scratch, n, io_.context)) {
        failed_ = true;
        return false;
      }
      same = std::memcmp(scratch, value.data() + offset, n) == 0;
      offset += n;
    }
    if (same) {
      ref = uint16_t(at);
      return true;
    }
    at += 2 + length;
  }
  ref = cursor_;
  binary::writeU16(scratch, uint16_t(value.size()));
  return append(scratch, 2) && append(value.data(), value.size());
}
bool Writer::club(uint16_t index, const Fields& fields) {
  if (failed_ || index != clubs_ ||
      index >= binary::readU16(&header_[kClubs]) || !seal()) {
    failed_ = true;
    return false;
  }
  const Field values[] = {fields.de, fields.en, fields.short_name,
                          fields.league};
  const std::size_t limits[] = {cfg::kTeamNameBytes, cfg::kTeamNameBytes,
                                cfg::kTeamShortBytes, SIZE_MAX};
  uint8_t bytes[9]{};
  for (unsigned n = 0; n < 4; ++n)
    if (values[n].present) {
      std::string_view adopted;
      uint16_t ref;
      if (!adopt(values[n].value, limits[n], adopted) ||
          !intern(adopted, ref)) {
        failed_ = true;
        return false;
      }
      binary::writeU16(bytes + n * 2, ref);
      bytes[8] |= uint8_t(1u << n);
    }
  if (!put(binary::readU16(&header_[kRecords]) + index * 9, bytes, 9))
    return false;
  ++clubs_;
  return true;
}
bool Writer::alias(uint16_t club_ref, std::string_view value) {
  if (failed_ || aliases_ >= binary::readU16(&header_[kAliases]) ||
      club_ref >= binary::readU16(&header_[kClubs]) || !seal()) {
    failed_ = true;
    return false;
  }
  uint16_t ref;
  if (!intern(value, ref)) return false;
  uint8_t bytes[4];
  binary::writeU16(bytes, club_ref);
  binary::writeU16(bytes + 2, ref);
  if (!put(binary::readU16(&header_[kAliasTable]) + aliases_ * 4, bytes, 4))
    return false;
  ++aliases_;
  return true;
}
bool Writer::exception(cfg::Provider provider, uint32_t id,
                       std::string_view value) {
  const auto p = static_cast<uint8_t>(provider);
  if (failed_ || exceptions_ >= binary::readU16(&header_[kExceptions]) ||
      p >= cfg::kProviderCount || !id ||
      (exceptions_ && (p < exception_provider_ ||
                       (p == exception_provider_ && id <= exception_id_))) ||
      !seal()) {
    failed_ = true;
    return false;
  }
  std::string_view clipped;
  uint16_t ref;
  if (!adopt(value, cfg::kTeamShortBytes, clipped) || !intern(clipped, ref)) {
    failed_ = true;
    return false;
  }
  uint8_t bytes[7];
  bytes[0] = p;
  binary::writeU32(bytes + 1, id);
  binary::writeU16(bytes + 5, ref);
  if (!put(binary::readU16(&header_[kExceptionTable]) + exceptions_ * 7, bytes,
           7))
    return false;
  ++exceptions_;
  exception_provider_ = p;
  exception_id_ = id;
  return true;
}
std::size_t Writer::finish() {
  if (!seal() || clubs_ != binary::readU16(&header_[kClubs]) ||
      aliases_ != binary::readU16(&header_[kAliases]) ||
      exceptions_ != binary::readU16(&header_[kExceptions])) {
    failed_ = true;
    return 0;
  }
  binary::writeU16(&header_[kTotal], cursor_);
  if (!put(0, header_.data(), header_.size())) return 0;
  uint8_t bytes[64];
  uint32_t crc = 0;
  for (std::size_t at = 0; at < cursor_;) {
    const auto n = std::min<std::size_t>(sizeof(bytes), cursor_ - at);
    if (!io_.read(at, bytes, n, io_.context)) {
      failed_ = true;
      return 0;
    }
    crc = binary::crc32({bytes, n}, crc);
    at += n;
  }
  binary::writeU32(bytes, crc);
  if (!put(kCrc, bytes, 4)) return 0;
  Reader checked;
  if (!checked.open(io_, cursor_)) {
    failed_ = true;
    return 0;
  }
  return cursor_;
}
}  // namespace football::names::store
