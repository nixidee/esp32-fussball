#include "file_candidate.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <utility>

#include "file_io.h"

namespace files {
static_assert(cfg::kImageMaxBytes <= std::numeric_limits<uint16_t>::max() &&
              cfg::kTeamNamesFileBytes <= std::numeric_limits<uint16_t>::max());
Candidate::~Candidate() { abort(); }
Candidate::Candidate(Candidate&& from) noexcept
    : context_(std::exchange(from.context_, nullptr)),
      fd_(std::exchange(from.fd_, -1)),
      error_(from.error_),
      extent_(from.extent_),
      limit_(from.limit_),
      location_(from.location_),
      flags_(std::exchange(from.flags_, 0)) {}
bool Candidate::fail(int error) {
  if (!error_) error_ = error ? error : EIO;
  errno = error_;
  return false;
}
bool Candidate::begin(const Context& context, Location file,
                      std::size_t limit) {
  if (busy()) {
    errno = EBUSY;  // Refusing another owner must not poison this candidate.
    return false;
  }
  error_ = 0;
  flags_ = 0;
  extent_ = 0;
  context_ = nullptr;
  const auto maximum =
      file.kind == FileKind::kImageCandidate ? cfg::kImageMaxBytes
      : file.kind == FileKind::kNamesSlot    ? cfg::kTeamNamesFileBytes
                                             : 0;
  if (!limit || limit > maximum) return fail(EINVAL);
  char path[kPathBufferBytes];
  if (!context.base_path || !ownedPath(context.base_path, file, path))
    return fail(EINVAL);
  // Recover only the private, unpublished image candidate. Names overwrite
  // their inactive slot only after admission; their active reference is safe.
  if (file.kind == FileKind::kImageCandidate && unlink(path) != 0 &&
      errno != ENOENT)
    return fail(errno);
  Usage usage;
  errno = 0;
  if (!context.usage || !context.usage(context.user, usage)) return fail(errno);
  if (usage.used_bytes > usage.total_bytes) return fail(EIO);
  if (!hasReplacementSpace(usage.total_bytes, usage.used_bytes, limit))
    return fail(ENOSPC);
  const int mode = O_RDWR | O_CREAT |
                   (file.kind == FileKind::kImageCandidate ? O_EXCL : O_TRUNC);
  fd_ = ::open(path, mode, 0600);
  if (fd_ < 0) return fail(errno);
  context_ = &context;
  location_ = file;
  limit_ = static_cast<uint16_t>(limit);
  flags_ = kOwned;
  return true;
}
bool Candidate::readAt(std::size_t at, void* bytes, std::size_t count) {
  if (error_) return fail(error_);
  if (fd_ < 0 || at > extent_ || count > extent_ - at) return fail(EINVAL);
  return readExact(fd_, at, bytes, count) || fail(errno);
}
bool Candidate::writeAt(std::size_t at, const void* bytes, std::size_t count) {
  if (error_) return fail(error_);
  if (fd_ < 0 || at > limit_ || count > limit_ - at) return fail(EINVAL);
  if (!writeExact(fd_, at, bytes, count)) return fail(errno);
  extent_ = static_cast<uint16_t>(std::max<std::size_t>(extent_, at + count));
  return true;
}
bool Candidate::complete(std::size_t size, Validate validate, void* raw) {
  if (fd_ < 0 || !busy() || validated()) return fail(EBADF);
  if (error_) {
    closeDescriptor(fd_);
    return fail(error_);
  }
  // An I/O failure takes priority over an incomplete caller stream.
  if (!syncAndClose(fd_)) return fail(errno);
  if (!size || size != extent_ || size > limit_ || !validate)
    return fail(EINVAL);
  char path[kPathBufferBytes];
  if (!ownedPath(context_->base_path, location_, path)) return fail(EINVAL);
  struct stat info{};
  if (stat(path, &info) != 0) return fail(errno);
  if (!S_ISREG(info.st_mode) || info.st_size != static_cast<off_t>(size))
    return fail(EILSEQ);
  errno = 0;
  if (!validate(path, raw)) return fail(errno ? errno : EILSEQ);
  flags_ |= kChecked;
  return true;
}
bool Candidate::publish(Location target) {
  if (error_) return fail(error_);
  if (!busy() || !validated() || location_.kind != FileKind::kImageCandidate ||
      target.kind != FileKind::kImageSlot)
    return fail(EINVAL);
  char from[kPathBufferBytes], to[kPathBufferBytes];
  if (!ownedPath(context_->base_path, location_, from) ||
      !ownedPath(context_->base_path, target, to))
    return fail(EINVAL);
  if (rename(from, to) != 0) return fail(errno);
  flags_ = 0;
  context_ = nullptr;
  return true;
}
void Candidate::preserve() {
  if (validated() && !error_) flags_ |= kRetained;
}
bool Candidate::abort() {
  bool okay = closeDescriptor(fd_);
  int cleanup_error = okay ? 0 : errno;
  if (!okay) fail(cleanup_error);
  if (busy() && !(flags_ & kRetained)) {
    char path[kPathBufferBytes];
    if (!ownedPath(context_->base_path, location_, path)) {
      okay = false;
      if (!cleanup_error) cleanup_error = EINVAL;
      fail(EINVAL);
    } else if (unlink(path) != 0 && errno != ENOENT) {
      okay = false;
      const int unlink_error = errno;
      if (!cleanup_error) cleanup_error = unlink_error;
      fail(unlink_error);
    }
  }
  flags_ = 0;
  context_ = nullptr;
  if (cleanup_error || error_) errno = cleanup_error ? cleanup_error : error_;
  return okay;
}
}  // namespace files
