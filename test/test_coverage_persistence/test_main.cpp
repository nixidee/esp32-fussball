#include <unity.h>

#include <array>

#include "schedule_coverage.h"

namespace {
namespace c = football::coverage;
constexpr uint32_t kNow = 1792238400, kDay = 86400;
c::Discovery discovery;
std::array<uint8_t, c::record::kGuardBytes> nvs;
c::DiscoveryRecord proposed, restored;

void erased_evidence_does_not_replenish_nvs_request_credits() {
  discovery.load(football::budget::Load::kMissing);
  discovery.start(kNow / kDay, proposed);
  discovery.commitStart(proposed);
  unsigned sent = 0;
  for (; sent < cfg::kCoverageRequestsPerRun; ++sent) {
    const auto admitted = discovery.request(proposed);
    if (admitted == c::Discovery::Admission::kStore) {
      TEST_ASSERT_EQUAL_UINT(c::record::kGuardBytes,
                             c::record::encodeGuard(proposed, nvs));
      discovery.commitRequest(proposed);
    } else {
      TEST_ASSERT_EQUAL(c::Discovery::Admission::kAllowed, admitted);
    }
  }
  c::Discovery reboot;
  TEST_ASSERT_EQUAL(c::record::DecodeResult::kOk,
                    c::record::decodeGuard(nvs, restored));
  reboot.load(football::budget::Load::kValid, restored);
  // No evidence file is supplied. Midnight during this same run must not
  // grant another group of sixteen requests either.
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kUsedUp, reboot.request(proposed));
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kActive,
                    reboot.start(kNow / kDay + 1, proposed));
}
void damaged_guard_does_not_modify_candidate_or_open_the_first_day() {
  discovery.load(football::budget::Load::kMissing);
  discovery.start(kNow / kDay, proposed);
  c::record::encodeGuard(proposed, nvs);
  restored.runs.day = 77;
  nvs.back() ^= 1;
  TEST_ASSERT_EQUAL(c::record::DecodeResult::kChecksum,
                    c::record::decodeGuard(nvs, restored));
  TEST_ASSERT_EQUAL_UINT32(77, restored.runs.day);
  discovery.load(football::budget::Load::kDamaged);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kUsedUp,
                    discovery.start(kNow / kDay, proposed));
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    discovery.start(kNow / kDay + 1, proposed));
}
void reboot_restarts_traversal_without_reopening_midnight_credits() {
  const auto day = kNow / kDay;
  discovery.load(football::budget::Load::kMissing);
  discovery.start(day, proposed);
  discovery.commitStart(proposed);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    discovery.request(proposed));
  discovery.commitRequest(proposed);
  TEST_ASSERT_EQUAL_UINT(c::record::kGuardBytes,
                         c::record::encodeGuard(discovery.stored(), nvs));
  TEST_ASSERT_EQUAL(c::record::DecodeResult::kOk,
                    c::record::decodeGuard(nvs, restored));
  c::Discovery reboot;
  reboot.load(football::budget::Load::kValid, restored);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kActive,
                    reboot.start(day + 1, proposed));
  unsigned sends = 1;
  for (;;) {
    const auto admission = reboot.request(proposed);
    if (admission == c::Discovery::Admission::kUsedUp) break;
    if (admission == c::Discovery::Admission::kStore)
      reboot.commitRequest(proposed);
    else
      TEST_ASSERT_EQUAL(c::Discovery::Admission::kAllowed, admission);
    ++sends;
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(cfg::kCoverageRequestsPerRun, sends);
    TEST_ASSERT_EQUAL_UINT32(day, reboot.stored().requests.day);
  }
  TEST_ASSERT_LESS_THAN_UINT32(cfg::kCoverageRequestsPerRun, sends);
  TEST_ASSERT_TRUE(reboot.finish(proposed));
  // A failed finish write cannot close the in-memory run optimistically.
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kActive,
                    reboot.start(day + 1, restored));
  reboot.commitFinish(proposed);
  c::record::encodeGuard(reboot.stored(), nvs);
  c::record::decodeGuard(nvs, restored);
  reboot.load(football::budget::Load::kValid, restored);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kUsedUp,
                    reboot.start(day, proposed));
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    reboot.start(day + 1, proposed));
}
void interrupted_or_obsolete_guard_never_replaces_the_callers_credit_state() {
  discovery.load(football::budget::Load::kMissing);
  discovery.start(kNow / kDay, proposed);
  c::record::encodeGuard(proposed, nvs);
  restored = proposed;
  for (std::size_t length = 0; length < nvs.size(); ++length) {
    TEST_ASSERT_EQUAL(c::record::DecodeResult::kLength,
                      c::record::decodeGuard({nvs.data(), length}, restored));
    TEST_ASSERT_EQUAL_UINT32(proposed.runs.day, restored.runs.day);
    TEST_ASSERT_TRUE(restored.active);
  }
  // This firmware accepts only the current guard, never a prior cursor codec.
  nvs[0] = 2;
  TEST_ASSERT_EQUAL(c::record::DecodeResult::kVersion,
                    c::record::decodeGuard(nvs, restored));
  discovery.load(football::budget::Load::kDamaged);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kUsedUp,
                    discovery.start(kNow / kDay, proposed));
}
}  // namespace

void setUp() {
  proposed = {};
  restored = {};
  nvs.fill(0);
}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(erased_evidence_does_not_replenish_nvs_request_credits);
  RUN_TEST(damaged_guard_does_not_modify_candidate_or_open_the_first_day);
  RUN_TEST(reboot_restarts_traversal_without_reopening_midnight_credits);
  RUN_TEST(
      interrupted_or_obsolete_guard_never_replaces_the_callers_credit_state);
  return UNITY_END();
}
