// Host checks of the per-provider daily request counter (OQ-163, OQ-170):
// stored record checks, the earlier single record, reservation, day change,
// a damaged record and a backward clock. They prove only the implementation
// of these rules, not the owner's device acceptance.

#include <unity.h>

#include <cstdint>

#include "app_config.h"
#include "budget_ledger.h"

namespace {

using football::budget::Counter;
using football::budget::LegacyRecord;
using football::budget::Load;
using football::budget::Record;
using Verdict = football::budget::Counter::Verdict;

constexpr uint32_t kDay = 20000;

// Admits one request and stores a reservation at once, like
// request_budget.cpp does when NVS accepts the record.
Verdict request(Counter& counter, uint32_t day, uint32_t limit,
                Record* stored = nullptr) {
  Record next{};
  const auto verdict = counter.admit(day, limit, &next);
  if (verdict == Verdict::kStore) {
    counter.commit(next);
    if (stored != nullptr) *stored = next;
  }
  return verdict;
}

void testRecordChecks() {
  Record out{};
  TEST_ASSERT_EQUAL(Load::kMissing,
                    football::budget::parse(nullptr, sizeof(Record), &out));
  const Record valid{cfg::kBudgetRecordMagic, kDay, 9};
  TEST_ASSERT_EQUAL(Load::kValid,
                    football::budget::parse(&valid, sizeof(valid), &out));
  TEST_ASSERT_EQUAL_UINT32(9, out.reserved);
  // W94: a record of the wrong length or with another magic is damaged.
  TEST_ASSERT_EQUAL(Load::kDamaged,
                    football::budget::parse(&valid, sizeof(valid) - 1, &out));
  const Record other{cfg::kBudgetLegacyMagic, kDay, 9};
  TEST_ASSERT_EQUAL(Load::kDamaged,
                    football::budget::parse(&other, sizeof(other), &out));
}

void testLegacyRecord() {
  const LegacyRecord legacy{cfg::kBudgetLegacyMagic, kDay, {1, 42, 3, 4}};
  Record out{};
  TEST_ASSERT_EQUAL(Load::kValid, football::budget::parseLegacy(
                                      &legacy, sizeof(legacy), 1, &out));
  TEST_ASSERT_EQUAL_HEX32(cfg::kBudgetRecordMagic, out.magic);
  TEST_ASSERT_EQUAL_UINT32(kDay, out.day);
  TEST_ASSERT_EQUAL_UINT32(42, out.reserved);
  TEST_ASSERT_EQUAL(Load::kDamaged, football::budget::parseLegacy(
                                        &legacy, sizeof(legacy) - 4, 1, &out));
  TEST_ASSERT_EQUAL(Load::kDamaged, football::budget::parseLegacy(
                                        &legacy, sizeof(legacy), 4, &out));
  LegacyRecord wrong = legacy;
  wrong.magic = cfg::kBudgetRecordMagic;
  TEST_ASSERT_EQUAL(Load::kDamaged, football::budget::parseLegacy(
                                        &wrong, sizeof(wrong), 1, &out));

  // The migrated count still limits today.
  Counter counter;
  football::budget::parseLegacy(&legacy, sizeof(legacy), 1, &out);
  counter.load(Load::kValid, out);
  TEST_ASSERT_EQUAL_UINT32(3, counter.remaining(45));
  for (int i = 0; i < 3; ++i)
    TEST_ASSERT_NOT_EQUAL(Verdict::kUsedUp, request(counter, kDay, 45));
  TEST_ASSERT_EQUAL(Verdict::kUsedUp, request(counter, kDay, 45));
}

void testReservationAndCap() {
  Counter counter;
  counter.load(Load::kMissing, {});
  Record stored{};
  // The first request stores a reservation of eight; the next seven are
  // admitted from RAM without a write.
  TEST_ASSERT_EQUAL(Verdict::kStore, request(counter, kDay, 10, &stored));
  TEST_ASSERT_EQUAL_UINT32(cfg::kBudgetReservationRequests, stored.reserved);
  TEST_ASSERT_EQUAL_UINT32(kDay, stored.day);
  for (unsigned i = 1; i < cfg::kBudgetReservationRequests; ++i)
    TEST_ASSERT_EQUAL(Verdict::kAllowed, request(counter, kDay, 10));
  TEST_ASSERT_EQUAL_UINT32(2, counter.remaining(10));
  // The last reservation stops at the cap.
  TEST_ASSERT_EQUAL(Verdict::kStore, request(counter, kDay, 10, &stored));
  TEST_ASSERT_EQUAL_UINT32(10, stored.reserved);
  TEST_ASSERT_EQUAL(Verdict::kAllowed, request(counter, kDay, 10));
  TEST_ASSERT_EQUAL(Verdict::kUsedUp, request(counter, kDay, 10));
  TEST_ASSERT_EQUAL_UINT32(0, counter.remaining(10));
  // A lower cap set during the day applies at once.
  Counter lower;
  lower.load(Load::kMissing, {});
  request(lower, kDay, 100);
  request(lower, kDay, 100);
  TEST_ASSERT_EQUAL(Verdict::kUsedUp, request(lower, kDay, 2));
}

void testRebootLosesUnusedReservation() {
  // A reboot reloads the stored record: reserved requests count as used.
  const Record stored{cfg::kBudgetRecordMagic, kDay, 8};
  Counter counter;
  counter.load(Load::kValid, stored);
  TEST_ASSERT_EQUAL_UINT32(2, counter.remaining(10));
  Record next{};
  TEST_ASSERT_EQUAL(Verdict::kStore, counter.admit(kDay, 10, &next));
  TEST_ASSERT_EQUAL_UINT32(10, next.reserved);
  // Without commit() (NVS refused the record) nothing is consumed.
  TEST_ASSERT_EQUAL_UINT32(2, counter.remaining(10));
}

void testNewDayAndBackwardClock() {
  Counter counter;
  counter.load(Load::kValid, {cfg::kBudgetRecordMagic, kDay, 10});
  TEST_ASSERT_EQUAL(Verdict::kUsedUp, request(counter, kDay, 10));
  // A day before the stored one counts against the stored day.
  TEST_ASSERT_EQUAL(Verdict::kUsedUp, request(counter, kDay - 1, 10));
  Record stored{};
  TEST_ASSERT_EQUAL(Verdict::kStore, request(counter, kDay + 1, 10, &stored));
  TEST_ASSERT_EQUAL_UINT32(kDay + 1, stored.day);
  TEST_ASSERT_EQUAL_UINT32(cfg::kBudgetReservationRequests, stored.reserved);
  TEST_ASSERT_EQUAL_UINT32(9, counter.remaining(10));
}

// OQ-163: a damaged record blocks only its provider, only for the day it is
// first asked about; the next day stores a new record.
void testDamagedRecord() {
  Counter counter;
  counter.load(Load::kDamaged, {});
  TEST_ASSERT_EQUAL_UINT32(0, counter.remaining(100));
  Record next{};
  TEST_ASSERT_EQUAL(Verdict::kUsedUp, counter.admit(kDay, 100, &next));
  TEST_ASSERT_EQUAL(Verdict::kUsedUp, counter.admit(kDay, 100, &next));
  TEST_ASSERT_EQUAL_UINT32(0, counter.remaining(100));
  Record stored{};
  TEST_ASSERT_EQUAL(Verdict::kStore, request(counter, kDay + 1, 100, &stored));
  TEST_ASSERT_EQUAL_UINT32(kDay + 1, stored.day);
  TEST_ASSERT_EQUAL_UINT32(cfg::kBudgetReservationRequests, stored.reserved);
  TEST_ASSERT_EQUAL_UINT32(99, counter.remaining(100));
  // Another provider's counter is independent.
  Counter other;
  other.load(Load::kMissing, {});
  TEST_ASSERT_EQUAL(Verdict::kStore, request(other, kDay, 100));
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(testRecordChecks);
  RUN_TEST(testLegacyRecord);
  RUN_TEST(testReservationAndCap);
  RUN_TEST(testRebootLosesUnusedReservation);
  RUN_TEST(testNewDayAndBackwardClock);
  RUN_TEST(testDamagedRecord);
  return UNITY_END();
}
