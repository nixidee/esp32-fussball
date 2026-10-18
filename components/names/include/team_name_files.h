// Two bounded immutable names versions. NVS AppConfig alone chooses the active
// version; this layer never publishes configuration or changes football RAM.
#pragma once
#include <cstddef>
#include <cstdint>

#include "file_candidate.h"
#include "names_ref.h"
#include "team_name_store.h"

namespace football::names::files {
using Usage = ::files::Usage;
using Context = ::files::Context;
enum class Status : uint8_t {
  kOk,
  kMissing,
  kInvalid,
  kIo,
  kConflict,
  kPhysicalSpace,
  kNoMemory
};
struct State {
  store::Reader checked;
  settings::NamesRef reference;
};
struct Receipt {
  Status status = Status::kInvalid;
  uint32_t generation = 0, digest = 0;
  bool activated = false;
};
class Candidate;
class Read {
 public:
  Read() = default;
  ~Read();
  Read(const Read&) = delete;
  Read& operator=(const Read&) = delete;
  Status open(const Context&, const State&);
  Status open(const Candidate&);
  const store::Reader& reader() const { return reader_; }
  bool close();

 private:
  int fd_ = -1;
  store::Reader reader_;
  Status open(const Context&, const settings::NamesRef&, const store::Reader&);
};
// Referenced missing/corrupt files fail honestly. No legacy lookup, repair,
// migration, implicit adoption or unreferenced-version scan.
Status load(const Context&, const settings::NamesRef&, State&);
class Candidate {
 public:
  Candidate() = default;
  ~Candidate();
  Candidate(Candidate&&) noexcept;
  Candidate(const Candidate&) = delete;
  Candidate& operator=(const Candidate&) = delete;
  // Caller holds shared heavy admission, excluding concurrent names readers.
  Status begin(const Context&, const State&);
  store::IO io();
  Status validate(std::size_t exact_size);
  settings::NamesRef reference() const;
  settings::NamesRef expectedReference() const { return expected_; }
  // Call only AFTER the one NVS config has durably committed reference().
  Receipt activate(State&);
  // Unknown NVS outcome: both versions must survive until reboot/recovery.
  void preserve();
  bool abort();
  const store::Reader& checked() const { return checked_; }
  Status failure() const;

 private:
  friend class Read;
  ::files::Candidate file_;
  settings::NamesRef expected_;
  store::Reader checked_;
  static bool readCandidate(std::size_t, void*, std::size_t, void*);
  static bool writeCandidate(std::size_t, const void*, std::size_t, void*);
};
}  // namespace football::names::files
