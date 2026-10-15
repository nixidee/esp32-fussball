#pragma once
// Daily request counter of one provider (pure logic, no NVS; the storage is
// request_budget.cpp). A request first reserves up to
// cfg::kBudgetReservationRequests requests in the stored record, so a reboot
// cannot replenish the day; unused reserved requests are lost at boot.
#include <cstddef>
#include <cstdint>

#include "app_config.h"

namespace football::budget {

// Stored record of one provider (12 bytes, cfg::kBudgetNvsKeys).
struct Record {
  uint32_t magic, day, reserved;
};
// The single record of earlier firmware: one day for all providers.
struct LegacyRecord {
  uint32_t magic, day, reserved[cfg::kProviderCount];
};

enum class Load : uint8_t { kMissing, kValid, kDamaged };
// Checks stored bytes; 'data' null = no record. On kValid *out holds it.
Load parse(const void* data, std::size_t length, Record* out);
// The earlier record's entry of 'provider', converted to a Record.
Load parseLegacy(const void* data, std::size_t length, unsigned provider,
                 Record* out);

class Counter {
 public:
  enum class Verdict : uint8_t {
    kAllowed,  // within the reserved requests, nothing to store
    kStore,    // store *next first, then call commit(*next)
    kUsedUp,   // the day's cap is reached
  };
  // A damaged record counts as used up for the first day it is asked about;
  // the next UTC day starts a new record (OQ-163).
  void load(Load state, const Record& record);
  // A damaged record becomes the used-up record of UTC 'day' (in RAM only),
  // so the next day has the whole limit again; admit() does this too.
  void settle(uint32_t day);
  // One request on UTC 'day' under the daily cap 'limit' (> 0). A day before
  // the stored one counts against the stored day (a backward clock
  // correction never reopens a day).
  Verdict admit(uint32_t day, uint32_t limit, Record* next);
  void commit(const Record& next);
  // Requests left today (0 while damaged or used up).
  uint32_t remaining(uint32_t limit) const;
  // Requests left on UTC 'day': 'limit' when 'day' is newer than the stored
  // day (no request on it yet), else remaining(limit).
  uint32_t remaining(uint32_t day, uint32_t limit) const;

 private:
  Record record_{cfg::kBudgetRecordMagic, 0, 0};
  uint32_t credits_ = 0;
  bool damaged_ = false;
};

}  // namespace football::budget
