// Pure helpers of the file service (docs/ARCHITECTURE.md, "File service").

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "app_config.h"

namespace files {
struct Usage {
  std::size_t total_bytes = 0, used_bytes = 0;
};
// Physical file identities only; content, image roles and active names
// references remain with their domain owners.
enum class FileKind : uint8_t {
  kNone,
  kImageCandidate,
  kImageSlot,
  kStockImage,
  kNamesSlot
};
struct Location {
  FileKind kind = FileKind::kNone;
  uint8_t slot = 0;
};
bool ownedName(Location file, std::span<char> out);
bool ownedPath(std::string_view base, Location file, std::span<char> out);

// "<base path>/<name>" plus terminator; sizeof(kFsBasePath) already counts
// one byte, which takes the separator.
inline constexpr std::size_t kPathBufferBytes =
    sizeof(cfg::kFsBasePath) + cfg::kFileNameMaxChars + 1;

// Name rule of cfg::kFileNameMaxChars (app_config.h).
bool isValidName(std::string_view name);

// Writes "<base>/<name>" with terminator into out. Returns false, with an
// empty string in out when it has room, for an invalid name or a too small
// buffer.
bool buildPath(std::string_view base, std::string_view name,
               std::span<char> out);

// True if every byte reads as erased NOR flash (0xFF); true for no bytes.
bool isErased(std::span<const uint8_t> bytes);

// One exclusive replacement workspace. The candidate consumes it; the reserve
// is not added again as untouched post-write space. Actual I/O can still fail
// for metadata/allocation and must preserve the published file on failure.
bool hasReplacementSpace(std::size_t total, std::size_t used,
                         std::size_t candidate);

}  // namespace files
