#include "file_io.h"

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <limits>

namespace files {
namespace {
bool seek(int fd, std::size_t offset) {
  if (offset > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
    errno = EOVERFLOW;
    return false;
  }
  const auto at = lseek(fd, static_cast<off_t>(offset), SEEK_SET);
  if (at == static_cast<off_t>(offset)) return true;
  if (at >= 0) errno = EIO;
  return false;
}
}  // namespace
bool readExact(int fd, std::size_t offset, void* bytes, std::size_t count) {
  if (!seek(fd, offset)) return false;
  auto* at = static_cast<uint8_t*>(bytes);
  while (count) {
    const auto got = ::read(fd, at, count);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) {
      if (got == 0) errno = EIO;
      return false;
    }
    at += got;
    count -= static_cast<std::size_t>(got);
  }
  return true;
}
bool writeExact(int fd, std::size_t offset, const void* bytes,
                std::size_t count) {
  if (!seek(fd, offset)) return false;
  auto* at = static_cast<const uint8_t*>(bytes);
  while (count) {
    const auto got = ::write(fd, at, count);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) {
      if (got == 0) errno = EIO;
      return false;
    }
    at += got;
    count -= static_cast<std::size_t>(got);
  }
  return true;
}
bool closeDescriptor(int& fd) {
  const int owned = fd;
  fd = -1;
  if (owned < 0) return true;
  errno = 0;
  const bool closed = ::close(owned) == 0;
  if (!closed && errno == 0) errno = EIO;
  return closed;
}
bool syncAndClose(int& fd) {
  if (fd < 0) {
    errno = EBADF;
    return false;
  }
  errno = 0;
  const bool synced = fsync(fd) == 0;
  const int sync_error = errno ? errno : EIO;
  const bool closed = closeDescriptor(fd);
  if (!synced) errno = sync_error;
  return synced && closed;
}
}  // namespace files
