#include "file_names.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace files {

namespace {

constexpr uint8_t kErasedByte = 0xFF;

bool isNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-' || c == '.';
}
bool join(std::string_view base, std::string_view name, std::span<char> out) {
  if (!out.empty()) out[0] = '\0';
  if (out.size() < 2 || name.size() > out.size() - 2 ||
      base.size() > out.size() - 2 - name.size())
    return false;
  const auto length = base.size() + 1 + name.size();
  std::memcpy(out.data(), base.data(), base.size());
  out[base.size()] = '/';
  std::memcpy(out.data() + base.size() + 1, name.data(), name.size());
  out[length] = '\0';
  return true;
}

}  // namespace

bool isValidName(std::string_view name) {
  if (name.empty() || name.size() > cfg::kFileNameMaxChars) return false;
  if (name.front() == '.') return false;
  return std::all_of(name.begin(), name.end(), isNameChar);
}

bool buildPath(std::string_view base, std::string_view name,
               std::span<char> out) {
  if (!out.empty()) out[0] = '\0';
  if (!isValidName(name)) return false;
  return join(base, name, out);
}
bool ownedName(Location file, std::span<char> out) {
  if (out.empty()) return false;
  out[0] = 0;
  const char* name = nullptr;
  switch (file.kind) {
    case FileKind::kImageCandidate:
      if (file.slot) return false;
      name = cfg::kImageTempFile;
      break;
    case FileKind::kImageSlot:
      if (file.slot == 0) return false;
      if (file.slot <= cfg::kImageLibraryMax) {
        const auto length = std::snprintf(out.data(), out.size(), "img%u.jpg",
                                          static_cast<unsigned>(file.slot));
        if (length >= 0 && static_cast<std::size_t>(length) < out.size())
          return true;
        out[0] = 0;
        return false;
      }
      if (file.slot > cfg::kImageLibraryMax + cfg::kStockImageCount)
        return false;
      name = cfg::kRoleImageFiles[file.slot - cfg::kImageLibraryMax - 1];
      break;
    case FileKind::kStockImage:
      if (file.slot >= cfg::kStockImageCount) return false;
      name = cfg::kStockImageFiles[file.slot];
      break;
    case FileKind::kNamesSlot:
      if (file.slot == 0 || file.slot > cfg::kTeamNamesSlotFiles.size())
        return false;
      name = cfg::kTeamNamesSlotFiles[file.slot - 1];
      break;
    case FileKind::kNone: return false;
  }
  if (!name) return false;
  const auto size = std::strlen(name);
  if (size >= out.size()) return false;
  std::memcpy(out.data(), name, size + 1);
  return true;
}
bool ownedPath(std::string_view base, Location file, std::span<char> out) {
  if (!out.empty()) out[0] = 0;
  char name[cfg::kFileNameMaxChars + 1];
  return ownedName(file, name) && join(base, name, out);
}

bool isErased(std::span<const uint8_t> bytes) {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](uint8_t b) { return b == kErasedByte; });
}

bool hasReplacementSpace(std::size_t total, std::size_t used,
                         std::size_t candidate) {
  return used <= total &&
         std::max(candidate, cfg::kFileReplacementWorkspaceBytes) <=
             total - used;
}

}  // namespace files
