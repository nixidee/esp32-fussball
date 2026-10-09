// Pure helpers of the file service (docs/ARCHITECTURE.md, "File service").

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "app_config.h"

namespace files {

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

}  // namespace files
