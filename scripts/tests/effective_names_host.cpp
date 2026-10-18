// Actual effective-label and explicit-presence code, synthetic names.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string_view>

#include "effective_names.h"

namespace {
bool watch = false;
std::size_t allocations = 0;
}  // namespace
void* operator new(std::size_t size) {
  if (watch) ++allocations;
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }

namespace {
namespace names = football::names;
namespace store = football::names::store;
using cfg::Provider;

void check(bool valid, const char* message) {
  if (!valid) throw std::runtime_error(message);
}

struct Memory {
  std::array<uint8_t, cfg::kTeamNamesFileBytes> bytes{};
  std::size_t used = 0, reads = 0, fail_read = 0;
  static bool read(std::size_t at, void* out, std::size_t count,
                   void* context) {
    auto& memory = *static_cast<Memory*>(context);
    if (++memory.reads == memory.fail_read || at > memory.used ||
        count > memory.used - at)
      return false;
    std::memcpy(out, memory.bytes.data() + at, count);
    return true;
  }
  static bool write(std::size_t at, const void* input, std::size_t count,
                    void* context) {
    auto& memory = *static_cast<Memory*>(context);
    if (at > memory.bytes.size() || count > memory.bytes.size() - at)
      return false;
    std::memcpy(memory.bytes.data() + at, input, count);
    if (memory.used < at + count) memory.used = at + count;
    return true;
  }
  store::IO io() { return {this, read, write}; }
};

void fixture(Memory& memory) {
  store::Writer writer;
  store::Counts counts;
  counts.clubs = 3;
  counts.bindings = {3, 1, 2, 1};
  counts.exceptions = 5;
  check(writer.begin(memory.io(), counts, 7, 0), "fixture begin");
  for (const auto key : {"alpha", "beta", "empty"})
    check(writer.key(key), "stable fixture key");
  check(writer.binding(Provider::kOpenLigaDb, 1, 0) &&
            writer.binding(Provider::kOpenLigaDb, 2, 1) &&
            writer.binding(Provider::kOpenLigaDb, 3, 2) &&
            writer.binding(Provider::kApiFootball, 77, 0) &&
            writer.binding(Provider::kEspn, 132, 0) &&
            writer.binding(Provider::kEspn, 999, 1) &&
            writer.binding(Provider::kFootballData, UINT32_MAX, 0),
        "all four providers and shared identities");
  check(
      writer.club(0, {{true, "Saved German"},
                      {true, "Saved English"},
                      {true, "ABC"},
                      {true, "Original league metadata"}}) &&
          writer.club(1, {{true, "Only German"}, {}, {}, {}}) &&
          writer.club(2, {{true, ""}, {true, "Other English"}, {true, ""}, {}}),
      "selected, absent and present-empty fields");
  check(writer.exception(Provider::kOpenLigaDb, 1, "ONE") &&
            writer.exception(Provider::kApiFootball, 88, "\xc3\x84\xc3\x96") &&
            writer.exception(Provider::kEspn, 132, "") &&
            writer.exception(Provider::kEspn, 133, "\xf0\x9f\x98\x80") &&
            writer.exception(Provider::kFootballData, UINT32_MAX, "XYZ"),
        "independent short exceptions including empty adopted prefixes");
  check(writer.finish() == memory.used, "complete checked fixture");
}

void effective(const store::Reader* reader, Provider provider, uint32_t id,
               uint8_t language, std::string_view expected_name,
               std::string_view expected_short,
               std::string_view original = "Provider Original",
               std::string_view original_short = "SRC") {
  names::Effective output;
  allocations = 0;
  watch = true;
  const bool ok = names::resolve(reader, provider, id, original, original_short,
                                 language, output);
  watch = false;
  check(ok, "effective resolution succeeds");
  check(allocations == 0, "effective resolution allocates no heap");
  check(std::string_view(output.name.data) == expected_name &&
            std::string_view(output.short_name.data) == expected_short,
        "owner-visible selected/fallback labels");
}

void selected_languages_and_id_exceptions() {
  Memory memory;
  fixture(memory);
  store::Reader reader;
  check(reader.open(memory.io(), memory.used), "checked names reader");
  effective(&reader, Provider::kOpenLigaDb, 1, 0, "Saved German", "ONE");
  effective(&reader, Provider::kOpenLigaDb, 1, 1, "Saved English", "ONE");
  effective(&reader, Provider::kApiFootball, 77, 0, "Saved German", "ABC");
  effective(&reader, Provider::kEspn, 132, 1, "Saved English", "");
  effective(&reader, Provider::kFootballData, UINT32_MAX, 0, "Saved German",
            "XYZ");
  effective(&reader, Provider::kApiFootball, 88, 0, "Provider Original",
            "\xc3\x84");
  effective(&reader, Provider::kEspn, 133, 0, "Provider Original", "");
  effective(&reader, Provider::kOpenLigaDb, 3, 0, "", "");
  effective(&reader, Provider::kOpenLigaDb, 3, 1, "Other English", "");
}

void absent_language_never_swaps_to_other_language() {
  Memory memory;
  fixture(memory);
  store::Reader reader;
  check(reader.open(memory.io(), memory.used), "checked names reader");
  effective(&reader, Provider::kEspn, 999, 0, "Only German", "SRC");
  effective(&reader, Provider::kEspn, 999, 1, "Provider Original", "SRC");
  // Neither a matching text nor an unknown provider/ID implicitly activates a
  // supplied/adopted club's other-language value.
  effective(&reader, Provider::kEspn, 123456, 1, "Saved German", "SRC",
            "Saved German");
  effective(nullptr, Provider::kEspn, 132, 1, "Provider Original", "SRC");
}

void raw_provider_fallback_and_full_input_validation() {
  effective(nullptr, Provider::kFootballData, UINT32_MAX, 1, "4294967295",
            "429", "", "");
  effective(nullptr, Provider::kEspn, 7, 0, "ABCD", "ABC", "", "ABCD");
  effective(nullptr, Provider::kEspn, 7, 0, "1234567890123456789012345", "123",
            "1234567890123456789012345\xc3\x84", "");
  effective(nullptr, Provider::kEspn, 7, 0, "Provider Original", "\xc3\x84",
            "Provider Original", "\xc3\x84\xc3\x96");
  Memory memory;
  fixture(memory);
  store::Reader reader;
  check(reader.open(memory.io(), memory.used), "checked names reader");
  names::Effective output;
  const char invalid_after_prefix[] = "123456789012345678901234567890\xc3";
  const char nul_after_prefix[] = "123456789012345678901234567890\0suffix";
  for (const auto raw :
       {std::string_view(invalid_after_prefix,
                         sizeof(invalid_after_prefix) - 1),
        std::string_view(nul_after_prefix, sizeof(nul_after_prefix) - 1)}) {
    check(!names::resolve(&reader, Provider::kOpenLigaDb, 1, raw, "SRC", 0,
                          output),
          "complete raw full name checked despite winning adopted field");
    check(!names::resolve(&reader, Provider::kOpenLigaDb, 1, "Provider", raw, 0,
                          output),
          "complete raw short checked despite winning adopted exception");
  }
  check(!names::resolve(nullptr, Provider::kEspn, 0, "A", "B", 0, output) &&
            !names::resolve(nullptr, static_cast<Provider>(99), 1, "A", "B", 0,
                            output) &&
            !names::resolve(nullptr, Provider::kEspn, 1, "A", "B", 2, output),
        "invalid identity/language fails closed");
}

bool adoptedFields(const store::Reader* reader, Provider provider, uint32_t id,
                   uint8_t language, bool& name, bool& short_name) {
  names::Effective value;
  return names::adopted(reader, provider, id, language, value, name,
                        short_name);
}

void provenance_prevents_saved_values_becoming_provider_originals() {
  Memory memory, empty;
  fixture(memory);
  store::Reader old, cleared;
  check(old.open(memory.io(), memory.used), "old adopted generation");
  store::Writer writer;
  check(writer.begin(empty.io(), {}, 8, 0) && writer.finish() > 0 &&
            cleared.open(empty.io(), empty.used),
        "cleared adopted generation");
  bool name = false, short_name = false;
  check(adoptedFields(&old, Provider::kOpenLigaDb, 1, 0, name, short_name) &&
            name && short_name,
        "old selected-name and short provenance recognized");
  // A materialized adopted value is not a retained provider original. With no
  // available raw provider data after removal, the correct fallback is the ID.
  effective(&cleared, Provider::kOpenLigaDb, 1, 0, "1", "1", "", "");
  effective(&cleared, Provider::kOpenLigaDb, 1, 0, "Provider Original", "SRC");
  check(adoptedFields(&old, Provider::kEspn, 999, 0, name, short_name) &&
            name && !short_name,
        "German adopted name does not mark provider short as adopted");
  check(adoptedFields(&old, Provider::kEspn, 999, 1, name, short_name) &&
            !name && !short_name,
        "missing English field is not marked by a German value");
  check(adoptedFields(&old, Provider::kEspn, 132, 1, name, short_name) &&
            name && short_name,
        "present-empty ID exception retains ownership provenance");
  check(adoptedFields(&old, Provider::kApiFootball, 88, 0, name, short_name) &&
            !name && short_name,
        "unknown club's ID short exception remains independent");
  check(
      adoptedFields(&cleared, Provider::kOpenLigaDb, 1, 0, name, short_name) &&
          !name && !short_name,
      "removed ownership absent in new generation");
}

void read_errors_fail_without_mutating_adopted_source() {
  Memory memory;
  fixture(memory);
  store::Reader reader;
  check(reader.open(memory.io(), memory.used), "checked names reader");
  const auto original = memory.bytes;
  names::Effective output;
  memory.reads = 0;
  check(names::resolve(&reader, Provider::kOpenLigaDb, 1, "Provider", "SRC", 0,
                       output),
        "resolution I/O baseline");
  const auto reads = memory.reads;
  for (std::size_t failure = 1; failure <= reads; ++failure) {
    memory.reads = 0;
    memory.fail_read = failure;
    check(!names::resolve(&reader, Provider::kOpenLigaDb, 1, "Provider", "SRC",
                          0, output),
          "every durable lookup/text read failure rejects prepared labels");
  }
  memory.reads = 0;
  memory.fail_read = 1;
  bool name = false, short_name = false;
  check(!adoptedFields(&reader, Provider::kOpenLigaDb, 1, 0, name, short_name),
        "failed provenance lookup cannot authorize fallback reuse");
  check(memory.bytes == original, "resolution never rewrites adopted data");
}

}  // namespace

int main() {
  try {
    selected_languages_and_id_exceptions();
    absent_language_never_swaps_to_other_language();
    raw_provider_fallback_and_full_input_validation();
    provenance_prevents_saved_values_becoming_provider_originals();
    read_errors_fail_without_mutating_adopted_source();
  } catch (const std::exception& error) {
    watch = false;
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "5 effective-label/presence/read-fault groups PASS; no global "
               "projection\n";
}
