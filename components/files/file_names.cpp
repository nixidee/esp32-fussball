#include "file_names.h"

#include <algorithm>
#include <cstring>

namespace files {

namespace {

constexpr uint8_t kErasedByte = 0xFF;

bool isNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-' || c == '.';
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
  const std::size_t length = base.size() + 1 + name.size();
  if (length + 1 > out.size()) return false;
  std::memcpy(out.data(), base.data(), base.size());
  out[base.size()] = '/';
  std::memcpy(out.data() + base.size() + 1, name.data(), name.size());
  out[length] = '\0';
  return true;
}

bool isErased(std::span<const uint8_t> bytes) {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](uint8_t b) { return b == kErasedByte; });
}

}  // namespace files
