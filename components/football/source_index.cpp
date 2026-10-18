#include "source_index.h"

namespace football {
bool SourceKey::valid() const {
  return static_cast<unsigned>(provider) < cfg::kProviderCount &&
         competition.length <= competition.kCapacity &&
         season.length <= season.kCapacity && !competition.empty() &&
         !season.empty() && values::isUtf8Text(competition.view()) &&
         values::isUtf8Text(season.view());
}
bool SourceKey::assign(cfg::Provider selected, std::string_view league,
                       std::string_view resolved_season) {
  SourceKey next;
  next.provider = selected;
  if (!next.competition.assign(league) ||
      !next.season.assign(resolved_season) || !next.valid())
    return false;
  *this = next;
  return true;
}
SourceId SourceIndex::find(const SourceKey& key) const {
  if (key.valid())
    for (const auto& entry : entries_)
      if (entry.id && entry.key == key) return entry.id;
  return {};
}
SourceId SourceIndex::intern(const SourceKey& key) {
  if (!key.valid()) return {};
  SourceId found = find(key);
  if (found || allocated_ == UINT32_MAX) return found;
  for (auto& entry : entries_)
    if (!entry.id) {
      entry.key = key;
      entry.id.value = ++allocated_;
      return entry.id;
    }
  return {};
}
SourceSlot SourceIndex::slot(SourceId id) const {
  if (id)
    for (SourceSlot i = 0; i < entries_.size(); ++i)
      if (entries_[i].id == id) return i;
  return kNoSource;
}
const SourceKey* SourceIndex::key(SourceSlot slot) const {
  return slot < entries_.size() && entries_[slot].id ? &entries_[slot].key
                                                     : nullptr;
}
SourceId SourceIndex::id(SourceSlot slot) const {
  return slot < entries_.size() ? entries_[slot].id : SourceId{};
}
void SourceIndex::release(SourceId id) {
  const auto at = slot(id);
  if (at != kNoSource) entries_[at] = {};
}
void SourceIndex::clear() {
  for (auto& entry : entries_) entry = {};
}
}  // namespace football
