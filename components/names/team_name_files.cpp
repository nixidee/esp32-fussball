#include "team_name_files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <utility>

#include "file_error.h"
#include "file_io.h"
#include "file_names.h"

namespace football::names::files {
namespace {
Status ioStatus(int error) {
  switch (::files::fromErrno(error)) {
    case ::files::Failure::kNone: return Status::kOk;
    case ::files::Failure::kCapacity: return Status::kPhysicalSpace;
    case ::files::Failure::kMemory: return Status::kNoMemory;
    case ::files::Failure::kIo: return Status::kIo;
  }
  return Status::kIo;
}
bool path(const Context& c, uint8_t slot, char* out, std::size_t capacity) {
  return slot >= 1 && slot <= cfg::kTeamNamesSlotFiles.size() &&
         ::files::buildPath(c.base_path, cfg::kTeamNamesSlotFiles[slot - 1],
                            {out, capacity});
}
bool fdRead(std::size_t at, void* bytes, std::size_t size, void* context) {
  return ::files::readExact(*static_cast<int*>(context), at, bytes, size);
}
bool checkedReference(const store::Reader& reader,
                      const settings::NamesRef& ref) {
  return reader.size() == ref.length && reader.generation() == ref.generation &&
         reader.digest() == ref.crc;
}
Status physical(const Context& c, std::size_t candidate) {
  Usage usage;
  if (!c.usage || !c.usage(c.user, usage) || usage.used > usage.total)
    return Status::kIo;
  // One shared replacement workspace. Do not reserve future unused image
  // quota, another untouched reserve, or an unconditional metadata proof.
  // I/O/close/validation failures preserve the referenced active version.
  return ::files::hasReplacementSpace(usage.total, usage.used, candidate)
             ? Status::kOk
             : Status::kPhysicalSpace;
}
}  // namespace

Read::~Read() { close(); }
bool Read::close() {
  reader_ = {};
  return ::files::closeDescriptor(fd_);
}
Status Read::open(const Context& context, const State& state) {
  return open(context, state.reference, state.checked);
}
Status Read::open(const Candidate& candidate) {
  if (!candidate.validated_ || !candidate.context_) return Status::kInvalid;
  return open(*candidate.context_, candidate.reference(), candidate.checked_);
}
Status Read::open(const Context& context, const settings::NamesRef& ref,
                  const store::Reader& checked) {
  if (!close()) return ioStatus(errno);
  if (!ref.slot) return Status::kMissing;
  char file[::files::kPathBufferBytes];
  if (!path(context, ref.slot, file, sizeof(file))) return Status::kInvalid;
  fd_ = ::open(file, O_RDONLY);
  if (fd_ < 0) return ioStatus(errno);
  struct stat info{};
  const bool exact = fstat(fd_, &info) == 0 && S_ISREG(info.st_mode) &&
                     info.st_size == ref.length;
  const bool valid =
      exact && checkedReference(checked, ref) &&
      reader_.reuse({&fd_, fdRead, nullptr}, checked, ref.length);
  if (valid) return Status::kOk;
  close();
  return Status::kInvalid;
}
Status load(const Context& context, const settings::NamesRef& ref,
            State& state) {
  state = {};
  state.reference = ref;
  if (!ref.slot)
    return ref == settings::NamesRef{} ? Status::kMissing : Status::kInvalid;
  char file[::files::kPathBufferBytes];
  if (!path(context, ref.slot, file, sizeof(file))) return Status::kInvalid;
  int fd = ::open(file, O_RDONLY);
  if (fd < 0)
    return ioStatus(errno);  // A referenced missing version is not empty names.
  struct stat info{};
  const bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                     info.st_size == ref.length &&
                     state.checked.open({&fd, fdRead, nullptr}, ref.length) &&
                     checkedReference(state.checked, ref);
  const bool closed = ::files::closeDescriptor(fd);
  const auto close_status = closed ? Status::kOk : ioStatus(errno);
  state.checked.detach();
  if (!valid || !closed) {
    state.checked = {};
    return valid ? close_status : Status::kInvalid;
  }
  return Status::kOk;
}

Candidate::~Candidate() { abort(); }
Candidate::Candidate(Candidate&& from) noexcept
    : context_(std::exchange(from.context_, nullptr)),
      fd_(std::exchange(from.fd_, -1)),
      expected_(from.expected_),
      checked_(from.checked_),
      slot_(from.slot_),
      failure_(from.failure_),
      validated_(from.validated_),
      retained_(from.retained_),
      owns_file_(std::exchange(from.owns_file_, false)) {
  checked_.detach();
}
Status Candidate::begin(const Context& context, const State& state) {
  if (context_ || state.reference.generation == UINT32_MAX)
    return Status::kConflict;
  const auto space = physical(context, cfg::kTeamNamesFileBytes);
  if (space != Status::kOk) return space;
  context_ = &context;
  expected_ = state.reference;
  slot_ = expected_.slot == 1 ? 2 : 1;
  char file[::files::kPathBufferBytes];
  if (!path(context, slot_, file, sizeof(file))) return Status::kInvalid;
  fd_ = ::open(file, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd_ < 0) return ioStatus(errno);
  owns_file_ = true;
  return Status::kOk;
}
bool Candidate::readCandidate(std::size_t at, void* out, std::size_t n,
                              void* raw) {
  auto& candidate = *static_cast<Candidate*>(raw);
  if (candidate.fd_ < 0) return false;
  if (fdRead(at, out, n, &candidate.fd_)) return true;
  if (candidate.failure_ == Status::kOk) candidate.failure_ = ioStatus(errno);
  return false;
}
bool Candidate::writeCandidate(std::size_t at, const void* bytes, std::size_t n,
                               void* raw) {
  auto& candidate = *static_cast<Candidate*>(raw);
  if (candidate.failure_ != Status::kOk) return false;
  if (candidate.validated_ || candidate.fd_ < 0 ||
      at > cfg::kTeamNamesFileBytes || n > cfg::kTeamNamesFileBytes - at) {
    candidate.failure_ = Status::kInvalid;
    return false;
  }
  if (::files::writeExact(candidate.fd_, at, bytes, n)) return true;
  candidate.failure_ = ioStatus(errno);
  return false;
}
store::IO Candidate::io() { return {this, readCandidate, writeCandidate}; }
Status Candidate::validate(std::size_t size) {
  if (failure_ != Status::kOk) return failure_;
  if (!context_ || fd_ < 0 || !size || size > cfg::kTeamNamesFileBytes)
    return Status::kInvalid;
  struct stat info{};
  if (fstat(fd_, &info) != 0) return failure_ = ioStatus(errno);
  if (info.st_size != static_cast<off_t>(size) || !checked_.open(io(), size) ||
      checked_.generation() != expected_.generation + 1u)
    return failure_ != Status::kOk ? failure_ : Status::kInvalid;
  const bool closed = ::files::syncAndClose(fd_);
  const auto close_status = closed ? Status::kOk : ioStatus(errno);
  checked_.detach();
  if (!closed) return failure_ = close_status;
  validated_ = true;
  return Status::kOk;
}
settings::NamesRef Candidate::reference() const {
  if (!validated_) return {};
  return {slot_, checked_.generation(), static_cast<uint16_t>(checked_.size()),
          checked_.digest()};
}
Receipt Candidate::activate(State& state) {
  Receipt receipt;
  if (!validated_ || state.reference != expected_) {
    receipt.status = Status::kConflict;
    return receipt;
  }
  state.reference = reference();
  state.checked = checked_;
  state.checked.detach();
  retained_ = true;
  receipt.status = Status::kOk;
  receipt.generation = checked_.generation();
  receipt.digest = checked_.digest();
  receipt.activated = true;
  return receipt;
}
void Candidate::preserve() { retained_ = true; }
bool Candidate::abort() {
  bool okay = ::files::closeDescriptor(fd_);
  if (context_ && owns_file_ && !retained_) {
    char file[::files::kPathBufferBytes];
    if (!path(*context_, slot_, file, sizeof(file)) ||
        (unlink(file) != 0 && errno != ENOENT))
      okay = false;
  }
  context_ = nullptr;
  owns_file_ = false;
  return okay;
}
}  // namespace football::names::files
