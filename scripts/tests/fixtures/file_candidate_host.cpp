// Whole production physical candidate; only POSIX faults/usage are adapted.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "file_candidate.h"

std::string root, expected;
std::size_t free_bytes = 1024 * 1024;
int writer_fd = -1, write_error = 0, sync_error = 0, close_error = 0;
int rename_error = 0, unlink_error = 0;
bool checked = false;
int test_open(const char* path, int flags, int mode) {
  writer_fd = ::open(path, flags, mode);
  return writer_fd;
}
ssize_t test_read(int fd, void* out, std::size_t count) {
  return ::read(fd, out, count);
}
ssize_t test_write(int fd, const void* bytes, std::size_t count) {
  if (write_error) {
    errno = write_error;
    return -1;
  }
  return ::write(fd, bytes, count);
}
int test_sync(int fd) {
  if (sync_error) {
    errno = sync_error;
    return -1;
  }
  return ::fsync(fd);
}
int test_close(int fd) {
  const int result = ::close(fd);
  if (close_error) {
    errno = close_error;
    return -1;
  }
  return result;
}
int test_rename(const char* from, const char* to) {
  if (rename_error) {
    errno = rename_error;
    return -1;
  }
  return ::rename(from, to);
}
int test_unlink(const char* path) {
  if (unlink_error) {
    errno = unlink_error;
    return -1;
  }
  return ::unlink(path);
}
std::string read(const std::string& path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), {}};
}
bool validate(const char* path, void*) {
  // Physical writer must already be closed, even before validation opens a
  // fresh reader. A readable active descriptor is not a completed candidate.
  assert(fcntl(writer_fd, F_GETFD) == -1 && errno == EBADF);
  checked = true;
  if (read(path) == expected) return true;
  errno = EILSEQ;
  return false;
}
int main(int argc, char** argv) {
  assert(argc == 2);
  root = argv[1];
  const files::Context context{root.c_str(), nullptr,
                               [](void*, files::Usage& out) {
                                 out = {1024 * 1024, 1024 * 1024 - free_bytes};
                                 return true;
                               }};
  const files::Location image{files::FileKind::kImageCandidate, 0};
  const files::Location slot{files::FileKind::kImageSlot, 1};
  const files::Location names{files::FileKind::kNamesSlot, 2};
  const auto target = root + "/img1.jpg";
  std::ofstream(target) << "old image";
  expected = "new image";
  {
    files::Candidate file;
    assert(file.begin(context, image, expected.size()));
    assert(file.writeAt(0, expected.data(), 3));
    assert(!file.begin(context, names, 9) && errno == EBUSY &&
           file.error() == 0);
    assert(file.writeAt(3, expected.data() + 3, expected.size() - 3));
    char prefix[3];
    assert(file.readAt(0, prefix, sizeof(prefix)));
    assert(std::string(prefix, sizeof(prefix)) == "new");
    assert(read(target) == "old image");
    assert(file.complete(expected.size(), validate, nullptr) && checked);
    assert(file.publish(slot));
    assert(read(target) == expected && !file.busy());
  }
  const auto active = read(target);
  for (unsigned fault = 0; fault < 5; ++fault) {
    checked = false;
    files::Candidate file;
    assert(file.begin(context, image, expected.size()));
    const auto size = fault == 0 ? 3 : expected.size();
    assert(file.writeAt(0, fault == 1 ? "bad image" : expected.data(), size));
    if (fault == 2) sync_error = EIO;
    if (fault == 2 || fault == 3) close_error = ENOMEM;
    if (fault == 4) rename_error = ENOSPC;
    const bool valid = file.complete(expected.size(), validate, nullptr);
    if (fault < 4) {
      assert(!valid && !file.validated());
      assert(checked == (fault == 1));
    } else
      assert(valid && !file.publish(slot));
    assert(file.error() == (fault == 0   ? EINVAL
                            : fault == 1 ? EILSEQ
                            : fault == 2 ? EIO
                            : fault == 3 ? ENOMEM
                                         : ENOSPC));
    sync_error = close_error = rename_error = 0;
    assert(file.abort());
    assert(read(target) == active);
  }
  {
    files::Candidate file;
    free_bytes = cfg::kFileReplacementWorkspaceBytes - 1;
    assert(!file.begin(context, image, 9) && file.error() == ENOSPC);
    free_bytes = cfg::kFileReplacementWorkspaceBytes;
    assert(file.begin(context, image, 9));
    write_error = ENOSPC;
    assert(!file.writeAt(0, expected.data(), expected.size()));
    write_error = 0;
    unlink_error = EIO;
    assert(!file.abort() && file.error() == ENOSPC && errno == EIO);
    unlink_error = 0;
    assert(read(target) == active);
    assert(file.begin(context, image, 9));  // Own stale candidate is recovered.
    assert(file.abort());
  }
  {
    files::Candidate original;
    assert(original.begin(context, names, cfg::kTeamNamesFileBytes));
    assert(original.writeAt(0, expected.data(), expected.size()));
    files::Candidate moved(std::move(original));
    assert(!original.busy() &&
           moved.complete(expected.size(), validate, nullptr));
    moved.preserve();  // Same operation for committed or uncertain NVS outcome.
  }
  assert(read(root + "/names_b.bin") == expected);
  assert(read(target) == active);
  for (const auto location :
       {files::Location{},
        files::Location{static_cast<files::FileKind>(255), 0},
        files::Location{files::FileKind::kNamesSlot, 0},
        files::Location{files::FileKind::kImageSlot, 255}}) {
    char path[files::kPathBufferBytes] = "unchanged";
    assert(!files::ownedPath(root, location, path) && path[0] == 0);
  }
  {
    files::Candidate file;
    assert(
        !file.begin(context, slot, 9));  // Active image slots are not writable.
    assert(read(target) == active);
  }
  std::puts(
      "PASS actual candidate: closed-before-check/atomic publish, stale owner "
      "refusal, bounded stream, first I/O failure, cleanup/recovery, "
      "workspace, moved/preserved names slot, typed locations");
}
