// Checked POSIX operations shared by bounded file owners. No heap or policy.
#pragma once
#include <cstddef>

namespace files {
int openRead(const char* path);
bool readExact(int fd, std::size_t offset, void* bytes, std::size_t count);
bool writeExact(int fd, std::size_t offset, const void* bytes,
                std::size_t count);
// Always invalidates the caller's descriptor; never retries close on EINTR.
bool closeDescriptor(int& fd);
// Close still runs when fsync fails, and both outcomes are checked.
// Failure keeps the first failing syscall's errno; EOF/zero writes use EIO.
bool syncAndClose(int& fd);
}  // namespace files
