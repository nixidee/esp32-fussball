#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <string>
#include <vector>

#include "football_model.h"
#include "team_name_json.h"
#include "team_name_service.h"

namespace names = football::names;
unsigned file_fault = 0;
int file_error = EIO;
int names_test_open(const char* path, int flags, int mode) {
  if (file_fault == 5) {
    file_fault = 0;
    errno = file_error;
    return -1;
  }
  return ::open(path, flags, mode);
}
std::size_t file_read_calls = 0;
ssize_t names_test_read(int fd, void* bytes, std::size_t size) {
  ++file_read_calls;
  if (file_fault == 1) {
    file_fault = 0;
    errno = file_error;
    return -1;
  }
  return ::read(fd, bytes, size);
}
ssize_t names_test_write(int fd, const void* bytes, std::size_t size) {
  if (file_fault == 2) {
    file_fault = 0;
    errno = file_error;
    return -1;
  }
  return ::write(fd, bytes, size);
}
int names_test_sync(int fd) {
  if (file_fault == 3) {
    file_fault = 0;
    errno = file_error;
    return -1;
  }
  return ::fsync(fd);
}
int names_test_close(int fd) {
  const auto status = ::close(fd);
  if (file_fault == 4) {
    file_fault = 0;
    errno = file_error;
    return -1;
  }
  return status;
}
namespace football::names {
files::Candidate* export_candidate = nullptr;
int Lookup::open() {
  present_ = read_.open(*export_candidate) == files::Status::kOk;
  return present_ ? 0 : -1;
}
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = -2,
              ESP_ERR_INVALID_RESPONSE = -3, ESP_ERR_INVALID_STATE = -4;
using esp_err_t = int;
// ACTUAL_EXPORT
}  // namespace football::names
struct Sink {
  std::string bytes;
  static bool write(std::string_view bytes, void* raw) {
    static_cast<Sink*>(raw)->bytes.append(bytes);
    return true;
  }
};
std::string storedText(const names::store::Reader& reader, uint16_t reference) {
  std::string text;
  assert(reader.streamText(
      reference,
      [](const void* bytes, std::size_t size, void* raw) {
        static_cast<std::string*>(raw)->append(static_cast<const char*>(bytes),
                                               size);
        return true;
      },
      &text));
  return text;
}
void sameCollection(const names::store::Reader& a,
                    const names::store::Reader& b) {
  const auto n = a.counts(), m = b.counts();
  assert(n.clubs == m.clubs && n.aliases == m.aliases &&
         n.exceptions == m.exceptions && n.bindings == m.bindings);
  for (uint16_t i = 0; i < n.clubs; ++i) {
    char ak[names::store::kKeyBytes + 1], bk[names::store::kKeyBytes + 1];
    names::store::Club ac, bc;
    assert(a.key(i, ak) && b.key(i, bk) && std::strcmp(ak, bk) == 0);
    assert(a.club(i, ac) && b.club(i, bc) && ac.present == bc.present);
    const uint16_t ar[] = {ac.de, ac.en, ac.short_name, ac.league},
                   br[] = {bc.de, bc.en, bc.short_name, bc.league};
    for (unsigned f = 0; f < 4; ++f)
      if (ac.present & (1u << f))
        assert(storedText(a, ar[f]) == storedText(b, br[f]));
    std::vector<std::string> aa, ba;
    for (uint16_t j = 0; j < n.aliases; ++j) {
      names::store::Alias x, y;
      assert(a.alias(j, x) && b.alias(j, y));
      if (x.club == i) aa.push_back(storedText(a, x.text));
      if (y.club == i) ba.push_back(storedText(b, y.text));
    }
    assert(aa == ba);
  }
  for (unsigned p = 0; p < cfg::kProviderCount; ++p)
    for (uint16_t i = 0; i < n.bindings[p]; ++i) {
      names::store::Binding x, y;
      const auto provider = static_cast<cfg::Provider>(p);
      assert(a.binding(provider, i, x) && b.binding(provider, i, y) &&
             x.id == y.id && x.club == y.club);
    }
  for (uint16_t i = 0; i < n.exceptions; ++i) {
    names::store::Exception x, y;
    assert(a.exception(i, x) && b.exception(i, y));
    assert(x.provider == y.provider && x.id == y.id &&
           storedText(a, x.text) == storedText(b, y.text));
  }
}
bool fail_scratch = false;
bool fail_export_index = false;
std::size_t export_index_bytes = 0;
void* operator new[](std::size_t count, const std::nothrow_t&) noexcept {
  export_index_bytes = std::max(export_index_bytes, count);
  if (fail_export_index) return nullptr;
  try {
    return ::operator new[](count);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}
std::size_t scratch_bytes = 0, scratch_allocations = 0;
void* operator new(std::size_t count, const std::nothrow_t&) noexcept {
  scratch_bytes = std::max(scratch_bytes, count);
  ++scratch_allocations;
  if (fail_scratch) return nullptr;
  try {
    return ::operator new(count);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}
struct Input {
  std::string_view text;
  std::size_t at = 0;
  static int read(void* raw) {
    auto& self = *static_cast<Input*>(raw);
    return self.at < self.text.size()
               ? static_cast<uint8_t>(self.text[self.at++])
               : -1;
  }
};
std::string all(const char* path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), {}};
}
int main(int argc, char** argv) {
  assert(argc >= 4);
  names::files::Context context{argv[1], nullptr,
                                [](void*, names::files::Usage& out) {
                                  out = {1048576, 0};
                                  return true;
                                }};
  {
    names::files::State published;
    {
      names::files::Candidate first;
      assert(first.begin(context, published) == names::files::Status::kOk);
      names::store::Writer writer;
      names::store::Counts counts;
      counts.clubs = 1;
      assert(writer.begin(first.io(), counts, 1, 0) && writer.key("kept"));
      names::store::Fields fields;
      fields.de = {true, "Kept"};
      assert(writer.club(0, fields));
      const auto size = writer.finish();
      assert(size && first.validate(size) == names::files::Status::kOk);
      assert(first.activate(published)
                 .activated);  // modeled successful NVS authority
    }
    const auto reference = published.reference;
    const auto active_path = std::filesystem::path(context.base_path) /
                             cfg::kTeamNamesSlotFiles[reference.slot - 1];
    const auto old = all(active_path.c_str());
    for (const int cause : {ENOSPC, ENOMEM, EIO}) {
      file_error = cause;
      for (unsigned fault = 1; fault <= 5; ++fault) {
        {
          names::files::Candidate candidate;
          if (fault == 5) {
            file_fault = fault;
            const auto expected =
                cause == ENOSPC   ? names::files::Status::kPhysicalSpace
                : cause == ENOMEM ? names::files::Status::kNoMemory
                                  : names::files::Status::kIo;
            assert(candidate.begin(context, published) == expected);
          } else {
            assert(candidate.begin(context, published) ==
                   names::files::Status::kOk);
            Input input{
                R"({"format":"football-team-names","version":1,"clubs":[{"key":"new","de":"New"}]})"};
            json::Reader reader({&input, Input::read});
            json::Token token;
            settings::Violation why;
            assert(reader.next(token));
            file_fault = fault;
            const auto expected = cause == ENOSPC ? names::Decode::kStorageFull
                                  : cause == ENOMEM ? names::Decode::kNoMemory
                                                    : names::Decode::kIo;
            assert(names::readAdoptedJson(reader, token, candidate, why) ==
                   expected);
          }
          assert(candidate.reference() == settings::NamesRef{} &&
                 published.reference == reference);
        }
        assert(file_fault == 0 && all(active_path.c_str()) == old);
        assert(
            !std::filesystem::exists(std::filesystem::path(context.base_path) /
                                     cfg::kTeamNamesSlotFiles[1]));
      }
    }
    file_error = EIO;
    std::filesystem::remove(active_path);
    std::cout << "candidate I/O: ENOSPC/ENOMEM/EIO open/read/write/sync/close "
                 "distinguish capacity/memory/I/O and preserve active "
                 "bytes/reference and abort inactive version PASS\n";
  }
  {
    Input input{"{}"};
    json::Reader reader({&input, Input::read});
    json::Token first;
    settings::Violation why;
    names::files::Candidate candidate;
    names::files::State active;
    assert(candidate.begin(context, active) == names::files::Status::kOk);
    assert(reader.next(first));
    fail_scratch = true;
    assert(names::readAdoptedJson(reader, first, candidate, why) ==
           names::Decode::kNoMemory);
    fail_scratch = false;
    assert(candidate.reference() == settings::NamesRef{});
  }
  for (int i = 2; i < argc; ++i) {
    const auto text = all(argv[i]);
    Input input{text};
    json::Reader reader({&input, Input::read});
    json::Token first;
    settings::Violation why;
    names::files::Candidate candidate;
    names::files::State active;
    assert(candidate.begin(context, active) == names::files::Status::kOk);
    auto status = names::Decode::kSyntax;
    if (reader.next(first))
      status = names::readAdoptedJson(reader, first, candidate, why);
    const bool complete = status == names::Decode::kOk && reader.finish();
    const bool expected =
        std::string_view(argv[i]).find("good-") != std::string_view::npos;
    if (complete != expected) {
      std::cerr << argv[i] << " status=" << int(status)
                << " path=" << why.path.data() << " reason=" << int(why.reason)
                << " reader=" << int(reader.error()) << '\n';
      return 1;
    }
    if (complete) {
      const auto receipt = candidate.reference();
      assert(receipt.slot == 1 && receipt.length <= cfg::kTeamNamesFileBytes &&
             receipt.generation == 1);
      names::files::Read checked;
      assert(checked.open(candidate) == names::files::Status::kOk);
      const auto& store = checked.reader();
      const auto counts = store.counts();
      const std::string name(argv[i]);
      if (name.find("preset") != std::string::npos) {
        assert(counts.clubs == 147 && counts.aliases == 113 &&
               counts.bindings[0] + counts.bindings[2] == 288);
      }
      if (name.find("long-duplicate") != std::string::npos) {
        names::store::Club club;
        names::store::Alias alias;
        assert(store.club(0, club) && counts.aliases == 2 && club.present == 8);
        assert(store.alias(0, alias) && alias.text == club.league);
        assert(store.alias(1, alias) && alias.text == club.league);
        std::size_t size = 0;
        assert(store.streamText(
                   alias.text,
                   [](const void*, std::size_t n, void* raw) {
                     *static_cast<std::size_t*>(raw) += n;
                     return true;
                   },
                   &size) &&
               size == 16260);
      }
      if (name.find("clipped") != std::string::npos) {
        names::store::Club club;
        assert(store.club(0, club) && club.present == 7);
        char out[40];
        std::size_t length;
        assert(store.text(club.de, out, sizeof(out), length) &&
               std::string_view(out, length) == std::string(26, 'a'));
        assert(store.text(club.en, out, sizeof(out), length) &&
               std::string_view(out, length) == "ä😀");
        assert(store.text(club.short_name, out, sizeof(out), length) &&
               length == 0);
      }
      if (name.find("empty") != std::string::npos) {
        names::store::Club club;
        assert(store.club(0, club) && club.present == 15 &&
               club.de == club.en && club.de == club.short_name &&
               club.de == club.league);
      }
      if (name.find("dense-ids") != std::string::npos)
        assert(counts.bindings[2] == 5500);
      if (name.find("keys") != std::string::npos) assert(counts.clubs == 1000);
      names::export_candidate = &candidate;
      Sink body;
      json::Writer writer({&body, Sink::write});
      names::AdoptedExport lease;
      const auto relations = counts.aliases + counts.bindings[0] +
                             counts.bindings[1] + counts.bindings[2] +
                             counts.bindings[3];
      if (relations) {
        fail_export_index = true;
        assert(lease.prepare() == names::ESP_ERR_NO_MEM && body.bytes.empty());
        fail_export_index = false;
        file_fault = 1;
        assert(lease.prepare() != 0 && body.bytes.empty());
      }
      file_read_calls = 0;
      assert(lease.prepare() == 0 && lease.write(writer) == 0 &&
             writer.finish());
      const auto export_reads = file_read_calls;
      assert(lease.write(writer) == names::ESP_ERR_INVALID_STATE);
      if (name.find("grouped") != std::string::npos) {
        assert(receipt.length == cfg::kTeamNamesFileBytes);
        assert(counts.clubs == 400 && counts.aliases == 2400 &&
               counts.bindings[2] == 400);
        assert(export_reads <
               128 * (std::size_t(counts.clubs) + relations + 1) +
                   store.size());
        std::cout << "linear grouped export reads=" << export_reads
                  << " former_alias_scans="
                  << std::size_t(counts.clubs) * counts.aliases << " index="
                  << 2 * (5 * std::size_t(counts.clubs) + relations) << "B\n";
      }
      assert(body.bytes.size() < cfg::kConfigBodyMaxBytes);
      if (name.find("many-aliases") != std::string::npos)
        assert(body.bytes.size() < 21000);
      if (name.find("keys") != std::string::npos)
        assert(body.bytes.size() > 107000);
      if (const auto* output = std::getenv("NAMES_EXPORT_DIR")) {
        const auto filename = std::filesystem::path(argv[i]).filename();
        std::ofstream saved(std::filesystem::path(output) / filename);
        saved << body.bytes;
        assert(saved.good());
      }
      const auto other_path = std::string(argv[1]) + "_out";
      std::filesystem::create_directory(other_path);
      auto other_context = context;
      other_context.base_path = other_path.c_str();
      names::files::Candidate restored;
      assert(restored.begin(other_context, active) ==
             names::files::Status::kOk);
      Input exported{body.bytes};
      json::Reader decoded({&exported, Input::read});
      json::Token value;
      assert(decoded.next(value) &&
             names::readAdoptedJson(decoded, value, restored, why) ==
                 names::Decode::kOk &&
             decoded.finish());
      names::files::Read restored_read;
      assert(restored_read.open(restored) == names::files::Status::kOk);
      sameCollection(store, restored_read.reader());
      assert(restored_read.close());
      assert(checked.close());
      std::cout << "PASS " << argv[i] << " file=" << receipt.length
                << " exported=" << body.bytes.size() << '\n';
    }
    assert(active.reference == settings::NamesRef{});  // parser never publishes
  }
  std::cout << "import scratch: one temporary allocation/request, host="
            << scratch_bytes << "B; requests=" << scratch_allocations
            << "; explicit OOM PASS\n";
  assert(export_index_bytes <= cfg::kTeamNamesFileBytes);
  std::cout << "export index: temporary only; largest request="
            << export_index_bytes
            << "B, preflight OOM before output and single-use lease PASS\n";
}
