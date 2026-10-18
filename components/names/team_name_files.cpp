#include "team_name_files.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

#include "file_error.h"
#include "file_io.h"
#include "file_names.h"

namespace football::names::files {
namespace {
Status ioStatus(int error) {
  if (error == EINVAL || error == EILSEQ) return Status::kInvalid;
  if (error == EBUSY) return Status::kConflict;
  switch (::files::fromErrno(error)) {
    case ::files::Failure::kNone: return Status::kOk;
    case ::files::Failure::kCapacity: return Status::kPhysicalSpace;
    case ::files::Failure::kMemory: return Status::kNoMemory;
    case ::files::Failure::kIo: return Status::kIo;
  }
  return Status::kIo;
}
bool path(const Context& c, uint8_t slot, char* out, std::size_t capacity) {
  return ::files::ownedPath(c.base_path, {::files::FileKind::kNamesSlot, slot},
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
  const auto* context = candidate.file_.context();
  if (!candidate.file_.validated() || !context) return Status::kInvalid;
  return open(*context, candidate.reference(), candidate.checked_);
}
Status Read::open(const Context& context, const settings::NamesRef& ref,
                  const store::Reader& checked) {
  if (!close()) return ioStatus(errno);
  if (!ref.slot) return Status::kMissing;
  char file[::files::kPathBufferBytes];
  if (!path(context, ref.slot, file, sizeof(file))) return Status::kInvalid;
  fd_ = ::files::openRead(file);
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
  int fd = ::files::openRead(file);
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

Candidate::~Candidate() = default;
Candidate::Candidate(Candidate&& from) noexcept
    : file_(std::move(from.file_)),
      expected_(from.expected_),
      checked_(from.checked_) {
  checked_.detach();
}
Status Candidate::begin(const Context& context, const State& state) {
  if (file_.context() || state.reference.generation == UINT32_MAX)
    return Status::kConflict;
  expected_ = state.reference;
  const uint8_t slot = expected_.slot == 1 ? 2 : 1;
  return file_.begin(context, {::files::FileKind::kNamesSlot, slot},
                     cfg::kTeamNamesFileBytes)
             ? Status::kOk
             : failure();
}
bool Candidate::readCandidate(std::size_t at, void* out, std::size_t n,
                              void* raw) {
  return static_cast<Candidate*>(raw)->file_.readAt(at, out, n);
}
bool Candidate::writeCandidate(std::size_t at, const void* bytes, std::size_t n,
                               void* raw) {
  return static_cast<Candidate*>(raw)->file_.writeAt(at, bytes, n);
}
store::IO Candidate::io() { return {this, readCandidate, writeCandidate}; }
Status Candidate::failure() const {
  return file_.error() ? ioStatus(file_.error()) : Status::kOk;
}
Status Candidate::validate(std::size_t size) {
  struct Check {
    Candidate* candidate;
    std::size_t size;
  } check{this, size};
  const bool valid = file_.complete(
      size,
      [](const char* path, void* raw) {
        const auto& check = *static_cast<Check*>(raw);
        auto& candidate = *check.candidate;
        int input = ::files::openRead(path);
        if (input < 0) return false;
        errno = 0;
        const bool valid =
            candidate.checked_.open({&input, fdRead, nullptr}, check.size) &&
            candidate.checked_.generation() ==
                candidate.expected_.generation + 1u;
        const int validation_error = errno ? errno : EILSEQ;
        const bool closed = ::files::closeDescriptor(input);
        candidate.checked_.detach();
        if (!valid) errno = validation_error;
        return valid && closed;
      },
      &check);
  return valid ? Status::kOk : failure();
}
settings::NamesRef Candidate::reference() const {
  if (!file_.validated()) return {};
  return {file_.location().slot, checked_.generation(),
          static_cast<uint16_t>(checked_.size()), checked_.digest()};
}
Receipt Candidate::activate(State& state) {
  Receipt receipt;
  if (!file_.validated() || state.reference != expected_) {
    receipt.status = Status::kConflict;
    return receipt;
  }
  state.reference = reference();
  state.checked = checked_;
  state.checked.detach();
  file_.preserve();
  receipt.status = Status::kOk;
  receipt.generation = checked_.generation();
  receipt.digest = checked_.digest();
  receipt.activated = true;
  return receipt;
}
void Candidate::preserve() { file_.preserve(); }
bool Candidate::abort() {
  checked_.detach();
  return file_.abort();
}
}  // namespace football::names::files
