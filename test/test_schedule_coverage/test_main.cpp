// Pure coverage contracts, not provider completeness or filesystem/device
// acceptance. Synthetic streams exercise the production collector and codec.
#include <unity.h>

#include <algorithm>
#include <array>
#include <limits>

#include "football_model.h"
#include "schedule_coverage.h"
#include "settings_record.h"

namespace {
namespace c = football::coverage;
constexpr uint32_t kDay = 86400;
constexpr uint32_t kNow = 1792238400;
c::EvidenceArray evidence, decoded;
settings::Competitions competitions;
c::Discovery discovery, rebooted;
c::DiscoveryRecord proposed, restored_guard;
std::array<uint8_t, c::record::kRecordBytes> bytes, second_bytes;

settings::Competition& addCompetition(uint8_t id) {
  settings::Competition entry;
  entry.id = id;
  entry.incarnation = 100 + id;
  entry.primary.competition.assign("bl1");
  entry.primary.season.assign("2026");
  competitions.entries.push(entry);
  competitions.next_incarnation = 101 + id;
  return competitions.entries[competitions.entries.size() - 1];
}

c::Integrity complete(uint32_t rows) { return {rows, true, true, true, true}; }

void putSpan(unsigned slot, uint32_t previous, uint32_t next, uint32_t from,
             uint32_t to, uint32_t now = kNow, const char* season = "2026") {
  const auto key = c::identity(competitions.entries[slot]);
  if (evidence[slot].id == 0) c::bind(evidence[slot], key);
  c::Collector collector;
  collector.begin(key, season, from, to, now, c::Proof::kDateRange);
  unsigned rows = 0;
  if (previous) {
    collector.observe(previous);
    ++rows;
  }
  if (next) {
    collector.observe(next);
    ++rows;
  }
  const auto& result = collector.finish(now, complete(rows));
  TEST_ASSERT_EQUAL(c::Completeness::kComplete, result.completeness);
  TEST_ASSERT_EQUAL(c::Reason::kNone, c::accept(evidence[slot], key, result));
}

void longGap(unsigned slot = 0) {
  putSpan(slot, kNow - 30 * kDay, kNow + 30 * kDay, kNow - 40 * kDay,
          kNow + 40 * kDay);
}

void complete_stream_evidence_does_not_depend_on_the_224_display_rows() {
  static_assert(cfg::kStoreMatches == 224);
  addCompetition(1);
  const auto key = c::identity(competitions.entries[0]);
  c::bind(evidence[0], key);
  c::Collector all, truncated;
  for (auto* collector : {&all, &truncated})
    collector->begin(key, "2026", kNow - 100 * kDay, kNow + 100 * kDay, kNow,
                     c::Proof::kFullSeason);
  // Both actual neighbours arrive AFTER the display store's row capacity.
  // The proof still observes them through its own compact stream reduction.
  for (unsigned row = 0; row < 306; ++row) {
    const uint32_t kickoff = row < 304    ? kNow - 60 * kDay + row
                             : row == 304 ? kNow - 30 * kDay
                                          : kNow + 30 * kDay;
    all.observe(kickoff);
    if (row < cfg::kStoreMatches) truncated.observe(kickoff);
  }
  const auto& result = all.finish(kNow, complete(306));
  TEST_ASSERT_EQUAL(c::Completeness::kComplete, result.completeness);
  TEST_ASSERT_EQUAL_UINT32(kNow - 30 * kDay, result.previous);
  TEST_ASSERT_EQUAL_UINT32(kNow + 30 * kDay, result.next);
  TEST_ASSERT_EQUAL(c::Reason::kNone, c::accept(evidence[0], key, result));
  TEST_ASSERT_EQUAL(c::Verdict::kBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    truncated.finish(kNow, complete(306)).completeness);
}

void every_active_competition_counts_even_without_club_or_enabled_screens() {
  addCompetition(1);
  auto& other = addCompetition(2);
  // Coverage has no screen-policy argument: every active competition counts.
  TEST_ASSERT_FALSE(other.hasClub());
  longGap();
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  putSpan(1, kNow - 2 * kDay, kNow + 2 * kDay, kNow - 40 * kDay,
          kNow + 40 * kDay);
  TEST_ASSERT_EQUAL(c::Verdict::kNoBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  other.active = false;
  TEST_ASSERT_EQUAL(c::Verdict::kBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
}

void complete_empty_competition_needs_other_real_global_neighbours() {
  addCompetition(1);
  putSpan(0, 0, 0, kNow - 40 * kDay, kNow + 40 * kDay);
  auto result = c::assess(evidence, competitions, kNow, 21);
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown, result.verdict);
  TEST_ASSERT_EQUAL(c::Reason::kMissingNeighbours, result.reason);
  addCompetition(2);
  longGap(1);
  TEST_ASSERT_EQUAL(c::Verdict::kBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
}

void strict_elapsed_day_threshold_and_dst_offsets_never_use_local_date_counts() {
  addCompetition(1);
  const auto run = [&](const char* before, const char* after, const char* now) {
    evidence = {};
    const auto previous = static_cast<uint32_t>(football::parseUtc(before));
    const auto next = static_cast<uint32_t>(football::parseUtc(after));
    const auto anchor = static_cast<uint32_t>(football::parseUtc(now));
    putSpan(0, previous, next, previous, next, anchor);
    return c::assess(evidence, competitions, anchor, 21).verdict;
  };
  // The same local time 21 calendar dates later is 20d23h across spring DST.
  TEST_ASSERT_EQUAL(c::Verdict::kNoBreak, run("2026-03-15T12:00:00+01:00",
                                              "2026-04-05T12:00:00+02:00",
                                              "2026-03-25T12:00:00+01:00"));
  TEST_ASSERT_EQUAL(c::Verdict::kNoBreak, run("2026-03-15T12:00:00+01:00",
                                              "2026-04-05T13:00:00+02:00",
                                              "2026-03-25T12:00:00+01:00"));
  TEST_ASSERT_EQUAL(c::Verdict::kBreak, run("2026-03-15T12:00:00+01:00",
                                            "2026-04-05T13:00:01+02:00",
                                            "2026-03-25T12:00:00+01:00"));
  // Autumn makes the same 21-date local span 21d1h, which exceeds the limit.
  TEST_ASSERT_EQUAL(c::Verdict::kBreak, run("2026-10-11T12:00:00+02:00",
                                            "2026-11-01T12:00:00+01:00",
                                            "2026-10-20T12:00:00+02:00"));
}

void freshness_expiry_and_backward_clock_are_explicit_unknowns() {
  addCompetition(1);
  longGap();
  TEST_ASSERT_EQUAL(
      c::Verdict::kBreak,
      c::assess(evidence, competitions, kNow + cfg::kCoverageMaxAgeS - 1, 21)
          .verdict);
  auto result =
      c::assess(evidence, competitions, kNow + cfg::kCoverageMaxAgeS, 21);
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown, result.verdict);
  TEST_ASSERT_EQUAL(c::Reason::kStale, result.reason);
  result = c::assess(evidence, competitions, kNow - 1, 21);
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown, result.verdict);
  TEST_ASSERT_EQUAL(c::Reason::kClock, result.reason);
}

void source_and_incarnation_changes_never_reuse_another_competitions_proof() {
  auto& competition = addCompetition(1);
  longGap();
  const auto hash = c::identity(competition).source;
  competition.primary.team.assign("40");
  TEST_ASSERT_NOT_EQUAL(hash, c::identity(competition).source);
  TEST_ASSERT_EQUAL(c::Reason::kIdentity,
                    c::assess(evidence, competitions, kNow, 21).reason);
  competition.primary.team.assign("");
  ++competition.incarnation;
  TEST_ASSERT_EQUAL(c::Reason::kIdentity,
                    c::assess(evidence, competitions, kNow, 21).reason);
}

void body_limit_pagination_missing_metadata_and_tbd_never_become_complete() {
  addCompetition(1);
  const auto key = c::identity(competitions.entries[0]);
  const auto prepare = [&](c::Collector& collector) {
    collector.begin(key, "2026", kNow - 40 * kDay, kNow + 40 * kDay, kNow,
                    c::Proof::kDateRange);
    collector.observe(kNow - 30 * kDay);
    collector.observe(kNow + 30 * kDay);
  };
  c::Collector collector;
  prepare(collector);
  collector.failed(c::Reason::kBodyLimit);
  TEST_ASSERT_EQUAL(c::Reason::kBodyLimit,
                    collector.finish(kNow, complete(2)).reason);
  prepare(collector);
  auto incomplete = complete(2);
  incomplete.pages_complete = false;
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    collector.finish(kNow, incomplete).completeness);
  prepare(collector);
  incomplete = complete(2);
  incomplete.metadata_verified = false;
  TEST_ASSERT_EQUAL(c::Reason::kMissingMetadata,
                    collector.finish(kNow, incomplete).reason);
  prepare(collector);
  collector.observe(0, false);
  TEST_ASSERT_EQUAL(c::Reason::kUndatedFixture,
                    collector.finish(kNow, complete(3)).reason);
  prepare(collector);
  collector.failed(c::Reason::kRequestLimit);
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    collector.finish(kNow, complete(2)).completeness);
}

void a_small_success_cannot_rejuvenate_season_proof_and_new_game_invalidates_it() {
  addCompetition(1);
  longGap();
  const auto key = c::identity(competitions.entries[0]);
  const auto checked = evidence[0].seasons[0].checked_utc;
  TEST_ASSERT_FALSE(c::observeNormal(evidence[0], key, kNow - 30 * kDay));
  TEST_ASSERT_EQUAL_UINT32(checked, evidence[0].seasons[0].checked_utc);
  TEST_ASSERT_EQUAL(
      c::Reason::kStale,
      c::assess(evidence, competitions, kNow + cfg::kCoverageMaxAgeS, 21)
          .reason);
  TEST_ASSERT_TRUE(c::observeNormal(evidence[0], key, kNow + 10 * kDay));
  TEST_ASSERT_EQUAL(c::Reason::kTimingChanged,
                    c::assess(evidence, competitions, kNow, 21).reason);
  TEST_ASSERT_EQUAL_UINT32(checked, evidence[0].seasons[0].checked_utc);
}

void two_seasons_join_only_through_proved_utc_adjacency_not_their_year_numbers() {
  addCompetition(1);
  const auto split = kNow - kDay;
  putSpan(0, kNow - 30 * kDay, 0, kNow - 40 * kDay, split, kNow, "2026");
  putSpan(0, 0, kNow + 30 * kDay, split + 1, kNow + 40 * kDay, kNow, "2027");
  TEST_ASSERT_EQUAL(c::Verdict::kBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  ++evidence[0].seasons[1].from;  // a single unproved UTC second is enough
  TEST_ASSERT_EQUAL(c::Reason::kUncovered,
                    c::assess(evidence, competitions, kNow, 21).reason);
  --evidence[0].seasons[1].from;
  const auto key = c::identity(competitions.entries[0]);
  auto third = evidence[0].seasons[1];
  third.season.assign("2028");
  TEST_ASSERT_EQUAL(c::Reason::kSeasonLimit,
                    c::accept(evidence[0], key, third));
}

void moving_past_a_remembered_neighbour_cannot_create_an_infinite_gap() {
  addCompetition(1);
  putSpan(0, kNow - 30 * kDay, kNow + 10, kNow - 40 * kDay, kNow + 40 * kDay);
  TEST_ASSERT_EQUAL(c::Verdict::kBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown,
                    c::assess(evidence, competitions, kNow + 11, 21).verdict);
}

void new_game_at_a_proved_season_boundary_also_invalidates_the_joined_gap() {
  addCompetition(1);
  const auto split = kNow - kDay;
  putSpan(0, kNow - 30 * kDay, 0, kNow - 40 * kDay, split, kNow, "2026");
  putSpan(0, 0, kNow + 30 * kDay, split + 1, kNow + 40 * kDay, kNow, "2027");
  TEST_ASSERT_EQUAL(c::Verdict::kBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  TEST_ASSERT_TRUE(c::observeNormal(
      evidence[0], c::identity(competitions.entries[0]), split));
  TEST_ASSERT_EQUAL(c::Reason::kTimingChanged,
                    c::assess(evidence, competitions, kNow, 21).reason);
}

void an_overlapping_season_cannot_hide_games_after_a_remembered_next_kickoff() {
  addCompetition(1);
  const auto key = c::identity(competitions.entries[0]);
  c::bind(evidence[0], key);
  c::Collector first;
  const auto collect = [&](uint32_t anchor) {
    first.begin(key, "2026", kNow - 40 * kDay, kNow + 40 * kDay, anchor,
                c::Proof::kFullSeason);
    first.observe(kNow - 30 * kDay);
    first.observe(kNow + 10);
    first.observe(kNow +
                  2 * kDay);  // streamed, but not the first future neighbour
    const auto& span = first.finish(anchor, complete(3));
    TEST_ASSERT_EQUAL(c::Completeness::kComplete, span.completeness);
    TEST_ASSERT_EQUAL(c::Reason::kNone, c::accept(evidence[0], key, span));
  };
  collect(kNow);
  putSpan(0, 0, kNow + 30 * kDay, kNow + 10, kNow + 40 * kDay, kNow, "2027");
  TEST_ASSERT_EQUAL(c::Verdict::kBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  const auto after = c::assess(evidence, competitions, kNow + 11, 21);
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown, after.verdict);
  TEST_ASSERT_EQUAL(c::Reason::kUncovered, after.reason);
  collect(kNow + 11);  // only a complete new query supplies the next real game
  TEST_ASSERT_EQUAL(c::Verdict::kNoBreak,
                    c::assess(evidence, competitions, kNow + 11, 21).verdict);
}

void partial_or_stale_overlapping_season_is_not_replaced_by_another_season() {
  addCompetition(1);
  longGap();
  const auto key = c::identity(competitions.entries[0]);
  auto partial = evidence[0].seasons[0];
  partial.completeness = c::Completeness::kPartial;
  partial.reason = c::Reason::kBodyLimit;
  TEST_ASSERT_EQUAL(c::Reason::kNone, c::accept(evidence[0], key, partial));
  putSpan(0, kNow - 30 * kDay, kNow + 30 * kDay, kNow - 40 * kDay,
          kNow + 40 * kDay, kNow, "2027");
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  longGap();
  evidence[0].seasons[0].checked_utc = kNow - cfg::kCoverageMaxAgeS;
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown,
                    c::assess(evidence, competitions, kNow, 21).verdict);
}

void normal_contradiction_stays_unknown_until_the_affected_season_is_reproved() {
  addCompetition(1);
  putSpan(0, kNow - 15 * kDay, kNow + 15 * kDay, kNow - 40 * kDay,
          kNow + 40 * kDay);
  putSpan(0, kNow - 30 * kDay, kNow + 30 * kDay, kNow - 40 * kDay,
          kNow + 40 * kDay, kNow, "2027");
  TEST_ASSERT_EQUAL(c::Verdict::kBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  TEST_ASSERT_TRUE(c::observeNormal(
      evidence[0], c::identity(competitions.entries[0]), kNow + kDay));
  for (const auto& span : evidence[0].seasons) {
    TEST_ASSERT_EQUAL(c::Completeness::kPartial, span.completeness);
    TEST_ASSERT_EQUAL(c::Reason::kTimingChanged, span.reason);
  }
  putSpan(0, kNow - 30 * kDay, kNow + 30 * kDay, kNow - 40 * kDay,
          kNow + 40 * kDay, kNow, "2027");
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  TEST_ASSERT_EQUAL_UINT32(
      bytes.size(), c::record::encode(evidence, discovery.stored(), bytes));
  TEST_ASSERT_EQUAL(c::record::DecodeResult::kOk,
                    c::record::decode(bytes, decoded, restored_guard));
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown,
                    c::assess(decoded, competitions, kNow, 21).verdict);
  putSpan(0, kNow - 15 * kDay, kNow + kDay, kNow - 40 * kDay, kNow + 40 * kDay);
  TEST_ASSERT_EQUAL(c::Verdict::kNoBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
}

void missing_refresh_metadata_cannot_be_cleared_by_another_seasons_success() {
  addCompetition(1);
  longGap();
  putSpan(0, kNow - 30 * kDay, kNow + 30 * kDay, kNow - 40 * kDay,
          kNow + 40 * kDay, kNow, "2027");
  const auto key = c::identity(competitions.entries[0]);
  c::Collector failed;
  failed.begin(key, "", kNow - 40 * kDay, kNow + 40 * kDay, kNow,
               c::Proof::kFullSeason);
  const auto& partial = failed.finish(kNow, complete(0));
  TEST_ASSERT_EQUAL(c::Reason::kMissingMetadata, partial.reason);
  TEST_ASSERT_EQUAL(c::Reason::kNone, c::accept(evidence[0], key, partial));
  TEST_ASSERT_EQUAL_UINT8(2, evidence[0].season_count);
  for (const auto& span : evidence[0].seasons) {
    TEST_ASSERT_EQUAL(c::Completeness::kPartial, span.completeness);
    TEST_ASSERT_EQUAL(c::Reason::kMissingMetadata, span.reason);
  }
  putSpan(0, kNow - 30 * kDay, kNow + 30 * kDay, kNow - 40 * kDay,
          kNow + 40 * kDay, kNow, "2027");
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown,
                    c::assess(evidence, competitions, kNow, 21).verdict);
  TEST_ASSERT_EQUAL_UINT32(
      bytes.size(), c::record::encode(evidence, discovery.stored(), bytes));
  TEST_ASSERT_EQUAL(c::record::DecodeResult::kOk,
                    c::record::decode(bytes, decoded, restored_guard));
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown,
                    c::assess(decoded, competitions, kNow, 21).verdict);
  longGap();
  TEST_ASSERT_EQUAL(c::Verdict::kBreak,
                    c::assess(evidence, competitions, kNow, 21).verdict);
}

void sixteen_device_requests_and_one_new_daily_run_require_persist_then_commit() {
  discovery.load(football::budget::Load::kMissing);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    discovery.start(100, proposed));
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kNoRun,
                    discovery.request(proposed));
  discovery.commitStart(proposed);
  unsigned stored = 0;
  for (unsigned i = 0; i < cfg::kCoverageRequestsPerRun; ++i) {
    const auto admission = discovery.request(proposed);
    TEST_ASSERT_TRUE(admission == c::Discovery::Admission::kAllowed ||
                     admission == c::Discovery::Admission::kStore);
    if (admission == c::Discovery::Admission::kStore) {
      ++stored;
      discovery.commitRequest(proposed);
    }
    auto progress = discovery.stored().progress;
    progress.route = i % cfg::kCompetitionMax;
    progress.season = i % cfg::kCoverageMaxSeasons;
    TEST_ASSERT_TRUE(discovery.stage(progress, proposed));
    discovery.commitProgress(proposed);
  }
  TEST_ASSERT_GREATER_THAN_UINT32(0, stored);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kUsedUp,
                    discovery.request(proposed));
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kActive,
                    discovery.start(101, proposed));
  auto progress = discovery.stored().progress;
  progress.season = cfg::kCoverageMaxSeasons;
  TEST_ASSERT_FALSE(discovery.stage(progress, proposed));
  progress.season = 0;
  progress.active = false;
  TEST_ASSERT_TRUE(discovery.stage(progress, proposed));
  discovery.commitProgress(proposed);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kUsedUp,
                    discovery.start(100, proposed));
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kUsedUp,
                    discovery.start(99, proposed));
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    discovery.start(101, proposed));
}

void reboot_loses_unused_reservations_and_never_replenishes_a_run() {
  discovery.load(football::budget::Load::kMissing);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    discovery.start(100, proposed));
  discovery.commitStart(proposed);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    discovery.request(proposed));
  discovery.commitRequest(proposed);  // the first actual request
  rebooted.load(football::budget::Load::kValid, discovery.stored());
  unsigned actual = 1;
  for (;;) {
    const auto admission = rebooted.request(proposed);
    if (admission == c::Discovery::Admission::kUsedUp) break;
    TEST_ASSERT_TRUE(admission == c::Discovery::Admission::kStore ||
                     admission == c::Discovery::Admission::kAllowed);
    if (admission == c::Discovery::Admission::kStore)
      rebooted.commitRequest(proposed);
    ++actual;
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(cfg::kCoverageRequestsPerRun, actual);
  }
  TEST_ASSERT_LESS_THAN_UINT32(cfg::kCoverageRequestsPerRun, actual);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kActive,
                    rebooted.start(102, proposed));
}

void corrupt_discovery_guard_cannot_be_saved_as_fresh_or_restarted_today() {
  discovery.load(football::budget::Load::kDamaged);
  TEST_ASSERT_FALSE(discovery.canPersist());
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kUsedUp,
                    discovery.start(100, proposed));
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kUsedUp,
                    discovery.start(100, proposed));
  TEST_ASSERT_FALSE(discovery.canPersist());
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    discovery.start(101, proposed));
  TEST_ASSERT_FALSE(discovery.canPersist());
  discovery.commitStart(proposed);
  TEST_ASSERT_TRUE(discovery.canPersist());
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    discovery.request(proposed));
}

void retrying_a_successful_start_commit_cannot_replenish_request_credits() {
  discovery.load(football::budget::Load::kMissing);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    discovery.start(100, proposed));
  const auto first_start = proposed;
  discovery.commitStart(first_start);
  for (unsigned i = 0; i < cfg::kCoverageRequestsPerRun; ++i) {
    const auto admission = discovery.request(proposed);
    if (admission == c::Discovery::Admission::kStore)
      discovery.commitRequest(proposed);
  }
  discovery.commitStart(first_start);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kUsedUp,
                    discovery.request(proposed));
  auto progress = discovery.stored().progress;
  progress.active = false;
  TEST_ASSERT_TRUE(discovery.stage(progress, proposed));
  discovery.commitProgress(proposed);
  discovery.commitStart(first_start);
  TEST_ASSERT_FALSE(discovery.stored().progress.active);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kUsedUp,
                    discovery.start(100, proposed));
}

void codec_is_bounded_crc_checked_and_candidate_only_without_snapshot_copy() {
  addCompetition(1);
  longGap();
  discovery.load(football::budget::Load::kMissing);
  TEST_ASSERT_EQUAL(c::Discovery::Admission::kStore,
                    discovery.start(100, proposed));
  discovery.commitStart(proposed);
  TEST_ASSERT_EQUAL_UINT32(
      bytes.size(), c::record::encode(evidence, discovery.stored(), bytes));
  TEST_ASSERT_EQUAL(c::record::DecodeResult::kOk,
                    c::record::decode(bytes, decoded, restored_guard));
  TEST_ASSERT_EQUAL_UINT32(
      bytes.size(), c::record::encode(decoded, restored_guard, second_bytes));
  TEST_ASSERT_EQUAL_MEMORY(bytes.data(), second_bytes.data(), bytes.size());
  bytes[20] ^= 1;
  decoded[0].incarnation = 777;
  restored_guard.progress.run_day = 888;
  TEST_ASSERT_EQUAL(c::record::DecodeResult::kChecksum,
                    c::record::decode(bytes, decoded, restored_guard));
  TEST_ASSERT_EQUAL_UINT32(777, decoded[0].incarnation);
  TEST_ASSERT_EQUAL_UINT32(888, restored_guard.progress.run_day);
  bytes = second_bytes;
  constexpr auto count_at = c::record::kHeaderBytes +
                            cfg::kCompetitionMax * c::record::kEvidenceBytes +
                            12 + 8;
  bytes[count_at] = cfg::kCoverageRequestsPerRun + 1;
  settings::seal(bytes, c::record::kVersion);
  TEST_ASSERT_EQUAL(c::record::DecodeResult::kInvalid,
                    c::record::decode(bytes, decoded, restored_guard));
  bytes.fill(0xa5);
  restored_guard.requests.reserved = cfg::kCoverageRequestsPerRun + 1;
  TEST_ASSERT_EQUAL_UINT32(0,
                           c::record::encode(evidence, restored_guard, bytes));
  TEST_ASSERT_EQUAL_HEX8(0xa5, bytes.front());
  TEST_ASSERT_EQUAL_HEX8(0xa5, bytes.back());
  TEST_ASSERT_EQUAL(c::record::DecodeResult::kLength,
                    c::record::decode(std::span<const uint8_t>(bytes).first(7),
                                      decoded, restored_guard));
}
}  // namespace

void setUp() {
  evidence = {};
  decoded = {};
  competitions.clear();
  proposed = {};
  restored_guard = {};
  discovery.load(football::budget::Load::kMissing);
  rebooted.load(football::budget::Load::kMissing);
  bytes.fill(0);
  second_bytes.fill(0);
}
void tearDown() {}
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(complete_stream_evidence_does_not_depend_on_the_224_display_rows);
  RUN_TEST(
      every_active_competition_counts_even_without_club_or_enabled_screens);
  RUN_TEST(complete_empty_competition_needs_other_real_global_neighbours);
  RUN_TEST(
      strict_elapsed_day_threshold_and_dst_offsets_never_use_local_date_counts);
  RUN_TEST(freshness_expiry_and_backward_clock_are_explicit_unknowns);
  RUN_TEST(
      source_and_incarnation_changes_never_reuse_another_competitions_proof);
  RUN_TEST(
      body_limit_pagination_missing_metadata_and_tbd_never_become_complete);
  RUN_TEST(
      a_small_success_cannot_rejuvenate_season_proof_and_new_game_invalidates_it);
  RUN_TEST(
      two_seasons_join_only_through_proved_utc_adjacency_not_their_year_numbers);
  RUN_TEST(moving_past_a_remembered_neighbour_cannot_create_an_infinite_gap);
  RUN_TEST(
      new_game_at_a_proved_season_boundary_also_invalidates_the_joined_gap);
  RUN_TEST(
      an_overlapping_season_cannot_hide_games_after_a_remembered_next_kickoff);
  RUN_TEST(
      partial_or_stale_overlapping_season_is_not_replaced_by_another_season);
  RUN_TEST(
      normal_contradiction_stays_unknown_until_the_affected_season_is_reproved);
  RUN_TEST(
      missing_refresh_metadata_cannot_be_cleared_by_another_seasons_success);
  RUN_TEST(
      sixteen_device_requests_and_one_new_daily_run_require_persist_then_commit);
  RUN_TEST(reboot_loses_unused_reservations_and_never_replenishes_a_run);
  RUN_TEST(corrupt_discovery_guard_cannot_be_saved_as_fresh_or_restarted_today);
  RUN_TEST(retrying_a_successful_start_commit_cannot_replenish_request_credits);
  RUN_TEST(
      codec_is_bounded_crc_checked_and_candidate_only_without_snapshot_copy);
  return UNITY_END();
}
