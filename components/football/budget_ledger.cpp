#include "budget_ledger.h"

#include <algorithm>
#include <cstring>

namespace football::budget {

Load parse(const void* data, std::size_t length, Record* out) {
  if (data == nullptr) return Load::kMissing;
  Record record{};
  if (length != sizeof(record)) return Load::kDamaged;
  std::memcpy(&record, data, sizeof(record));
  if (record.magic != cfg::kBudgetRecordMagic) return Load::kDamaged;
  *out = record;
  return Load::kValid;
}

Load parseLegacy(const void* data, std::size_t length, unsigned provider,
                 Record* out) {
  if (data == nullptr) return Load::kMissing;
  LegacyRecord legacy{};
  if (length != sizeof(legacy) || provider >= cfg::kProviderCount)
    return Load::kDamaged;
  std::memcpy(&legacy, data, sizeof(legacy));
  if (legacy.magic != cfg::kBudgetLegacyMagic) return Load::kDamaged;
  *out = {cfg::kBudgetRecordMagic, legacy.day, legacy.reserved[provider]};
  return Load::kValid;
}

void Counter::load(Load state, const Record& record) {
  record_ =
      state == Load::kValid ? record : Record{cfg::kBudgetRecordMagic, 0, 0};
  credits_ = 0;
  damaged_ = state == Load::kDamaged;
}

Counter::Verdict Counter::admit(uint32_t day, uint32_t limit, Record* next) {
  if (damaged_) {
    // Used up for this day, in RAM only: nothing is stored until the next
    // day writes a new record.
    damaged_ = false;
    record_ = {cfg::kBudgetRecordMagic, day, UINT32_MAX};
    credits_ = 0;
  }
  const bool new_day = day > record_.day;
  if (!new_day && credits_ > 0 && record_.reserved - credits_ < limit) {
    --credits_;
    return Verdict::kAllowed;
  }
  Record base = new_day ? Record{cfg::kBudgetRecordMagic, day, 0} : record_;
  if (base.reserved >= limit) return Verdict::kUsedUp;
  base.reserved += std::min<uint32_t>(cfg::kBudgetReservationRequests,
                                      limit - base.reserved);
  *next = base;
  return Verdict::kStore;
}

void Counter::commit(const Record& next) {
  const uint32_t before = next.day == record_.day ? record_.reserved : 0;
  record_ = next;
  credits_ = next.reserved - before - 1;
}

uint32_t Counter::remaining(uint32_t limit) const {
  if (damaged_) return 0;
  const uint32_t used = record_.reserved - credits_;
  return used >= limit ? 0 : limit - used;
}

uint32_t Counter::remaining(uint32_t day, uint32_t limit) const {
  if (!damaged_ && day > record_.day) return limit;
  return remaining(limit);
}

}  // namespace football::budget
