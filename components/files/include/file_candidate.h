// One bounded physical candidate. Domain owners supply content validation;
// image publication and NVS reference activation remain distinct operations.
#pragma once
#include "file_names.h"

namespace files {
struct Context {
  const char* base_path = cfg::kFsBasePath;
  void* user = nullptr;
  bool (*usage)(void*, Usage&) = nullptr;
};
class Candidate {
 public:
  using Validate = bool (*)(const char* closed_path, void* context);
  Candidate() = default;
  ~Candidate();
  Candidate(Candidate&&) noexcept;
  Candidate(const Candidate&) = delete;
  Candidate& operator=(const Candidate&) = delete;
  // Context must outlive the candidate. Only image candidates and names
  // slots are writable; their logical bounds come from the file type.
  bool begin(const Context&, Location, std::size_t limit);
  bool readAt(std::size_t offset, void*, std::size_t count);
  bool writeAt(std::size_t offset, const void*, std::size_t count);
  // Requires exact extent and successful sync/close before validation.
  bool complete(std::size_t exact_size, Validate, void* context);
  // Only a checked image candidate can rename onto an image slot. Caller
  // holds the image-reader barrier. Names use preserve() after NVS instead.
  bool publish(Location target);
  void preserve();
  // On cleanup failure errno describes cleanup; error() retains the first
  // operation failure, so callers can report both without changing its cause.
  bool abort();
  bool busy() const { return flags_ & kOwned; }
  bool validated() const { return (flags_ & kChecked) && error_ == 0; }
  const Context* context() const { return context_; }
  Location location() const { return location_; }
  std::size_t size() const { return extent_; }
  std::size_t limit() const { return limit_; }
  int error() const { return error_; }

 private:
  enum : uint8_t { kOwned = 1, kChecked = 2, kRetained = 4 };
  const Context* context_ = nullptr;
  int fd_ = -1, error_ = 0;
  uint16_t extent_ = 0, limit_ = 0;
  Location location_;
  uint8_t flags_ = 0;
  bool fail(int error);
};
}  // namespace files
