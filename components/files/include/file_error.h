// Portable filesystem failure categories. RAM exhaustion and exhausted file
// capacity need different user actions; neither is a generic I/O success.
#pragma once
#include <cerrno>
#include <cstdint>

namespace files {
// Project-local application range, outside the SDK's component error ranges.
// Fits signed32 esp_err_t; defined once for services and the HTTP fault mapper.
inline constexpr int kStorageFull = 0x7f000001;
enum class Failure : uint8_t { kNone, kIo, kCapacity, kMemory };
constexpr Failure fromErrno(int error) noexcept {
  return error == ENOSPC   ? Failure::kCapacity
         : error == ENOMEM ? Failure::kMemory
                           : Failure::kIo;
}
}  // namespace files
