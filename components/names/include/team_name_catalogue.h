#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace football::names::catalogue {
inline constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);
inline constexpr std::size_t kAmbiguous = kNotFound - 1;

// Strings and ID spans refer directly to the immutable firmware source.
// Constructing a view allocates nothing and never adopts/clips a label.
struct ClubView {
  std::string_view key, league, de, en, short_name;
  std::span<const uint32_t> espn_ids, openligadb_ids;
  std::size_t index{kNotFound};
  std::size_t aliasCount() const;
  std::string_view alias(std::size_t alias_index) const;
};

std::size_t count();
ClubView at(std::size_t index);
std::size_t findKey(std::string_view key);
std::size_t findId(std::string_view provider, uint32_t id);
// Exact full DE/EN name or alias; duplicate labels within one club count once.
// Ambiguous text never chooses a club. Provider/ID matching precedes this call.
std::size_t uniqueExact(std::string_view original_name);
const char* sourceSha256();
std::size_t payloadBytes();

// Streams the full source schema as UTF-8 JSON, with a bounded 128-byte buffer.
// False from the callback cancels immediately. No heap, filesystem or inflater.
using Write = bool (*)(void* context, const char* data, std::size_t length);
bool streamJson(Write write, void* context);
}  // namespace football::names::catalogue
