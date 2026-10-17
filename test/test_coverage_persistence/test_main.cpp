#include <unity.h>

#include <array>

#include "schedule_coverage.h"

namespace {
namespace c = football::coverage;
constexpr uint32_t kNow = 1792238400, kDay = 86400;
c::EvidenceArray evidence, rebooted;
settings::Competitions competitions;
c::Discovery discovery;
std::array<uint8_t, c::record::kRecordBytes> file;
std::array<uint8_t, c::record::kGuardBytes> nvs;
c::DiscoveryRecord proposed, restored;

void proof(uint8_t id) {
  settings::Competition entry;
  entry.id = id;
  entry.incarnation = id;
  entry.primary.competition.assign("bl1");
  entry.primary.season.assign("2026");
  competitions.entries.push(entry);
  const auto key = c::identity(entry);
  auto& row = evidence[id - 1];
  c::bind(row, key);
  c::Collector collector;
  collector.begin(key, "2026", kNow - 40 * kDay, kNow + 40 * kDay, kNow,
                  c::Proof::kDateRange);
  collector.observe(kNow - 30 * kDay);
  collector.observe(kNow + 30 * kDay);
  c::accept(row, key, collector.finish(kNow, {2, true, true, true, true}));
}
void fence_survives_reset_before_first_or_last_response_row() {
  proof(1);
  TEST_ASSERT_EQUAL(c::Verdict::kBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  TEST_ASSERT_EQUAL_UINT(c::record::kRecordBytes,
                         c::record::encode(evidence, {}, file, 1));
  TEST_ASSERT_EQUAL(c::record::DecodeResult::kOk,
                    c::record::decode(file, rebooted, restored));
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown,
                    c::assess(rebooted, competitions, kNow, 21).verdict);
  // A complete successful response with no contradiction may restore exactly
  // the old evidence; encoding its fence never changed the retained owner.
  TEST_ASSERT_EQUAL(c::Verdict::kBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  c::record::encode(evidence, {}, file);
  c::record::decode(file, rebooted, restored);
  TEST_ASSERT_EQUAL(c::Verdict::kBreak,
                    c::assess(rebooted, competitions, kNow, 21).verdict);
}
void fence_invalidates_only_the_named_identity_without_changing_guard() {
  proof(1);
  proof(2);
  discovery.load(football::budget::Load::kMissing);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    discovery.start(kNow / kDay, proposed));
  discovery.commitStart(proposed);
  c::record::encode(evidence, discovery.stored(), file, 2);
  c::record::decode(file, rebooted, restored);
  TEST_ASSERT_EQUAL(c::Completeness::kComplete,
                    rebooted[0].seasons[0].completeness);
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    rebooted[1].seasons[0].completeness);
  TEST_ASSERT_EQUAL_UINT32(discovery.stored().progress.run_day,
                           restored.progress.run_day);
}
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
  restored.progress.step = 77;
  nvs.back() ^= 1;
  TEST_ASSERT_EQUAL(c::record::DecodeResult::kChecksum,
                    c::record::decodeGuard(nvs, restored));
  TEST_ASSERT_EQUAL_UINT8(77, restored.progress.step);
  discovery.load(football::budget::Load::kDamaged);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kUsedUp,
                    discovery.start(kNow / kDay, proposed));
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    discovery.start(kNow / kDay + 1, proposed));
}
}  // namespace

void setUp() {
  evidence = {};
  rebooted = {};
  competitions.clear();
  proposed = {};
  restored = {};
  file.fill(0);
  nvs.fill(0);
}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(fence_survives_reset_before_first_or_last_response_row);
  RUN_TEST(fence_invalidates_only_the_named_identity_without_changing_guard);
  RUN_TEST(erased_evidence_does_not_replenish_nvs_request_credits);
  RUN_TEST(damaged_guard_does_not_modify_candidate_or_open_the_first_day);
  return UNITY_END();
}
