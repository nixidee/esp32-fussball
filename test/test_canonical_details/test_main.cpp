// Targeted acceptance for the approved canonical detail/time additions.
// Synthetic cases prove bounded storage and transitions. The recorded ESPN
// replay proves parser parity for that historical response, not current public
// delivery, endpoint demand or physical-device acceptance.
#include <ArduinoJson.h>
#include <unity.h>

#include <cstring>

#include "../provider_adapter_support.h"
#include "../provider_status_support.h"
#include "../test_match_store/recorded.h"
#include "match_store.h"
#include "store_views.h"

namespace {
using namespace football;
constexpr auto kProvider = cfg::Provider::kEspn;
constexpr uint32_t kNow = 1792245600;
MatchStore store;

StoredMatch* add(uint8_t competition, uint32_t id, bool own,
                 uint32_t kickoff = kNow) {
  StoredMatch m;
  m.id = id;
  m.competition = competition;
  m.provider = kProvider;
  m.kickoff = kickoff;
  m.phase = Phase::kLive;
  m.home = store.team(competition, kProvider, id * 2, "Ordinary home");
  m.away = store.team(competition, kProvider, id * 2 + 1, "Ordinary away");
  if (own) m.flags = kMatchOwn;
  return store.upsert(m, {kNow, 100});
}

const StoredMatch& find(uint8_t competition, uint32_t id) {
  const auto* m = store.find(competition, kProvider, id);
  TEST_ASSERT_NOT_NULL(m);
  return *m;
}

void fillEvents(uint8_t competition, uint32_t id, uint32_t seed) {
  const auto& m = find(competition, id);
  TEST_ASSERT_TRUE(store.beginEvents(m, false, kNow));
  for (unsigned i = 0; i < cfg::kMatchEvents; ++i) {
    StoredEvent e;
    e.identity = seed + i;
    e.minute = i;
    e.player.assign("Scorer");
    store.addEvent(e);
  }
  store.endEvents();
}

void test_ordinary_details_require_an_explicit_identity() {
  add(1, 10, false);
  TEST_ASSERT_FALSE(store.hasDetail(find(1, 10)));
  auto* m = store.ensureDetail(1, kProvider, 10, {kNow, 100});
  TEST_ASSERT_NOT_NULL(m);
  TEST_ASSERT_FALSE(m->isOwn());
  TEST_ASSERT_TRUE(m->detail >= cfg::kStoreOwnMatches);
  TEST_ASSERT_EQUAL_STRING("Ordinary home", view::teamName(store, *m, true));
  fillEvents(1, 10, 100);
  TEST_ASSERT_EQUAL_UINT8(12, view::events(store, find(1, 10)).count);
  add(1, 10, false);
  TEST_ASSERT_TRUE(store.hasDetail(find(1, 10)));
  TEST_ASSERT_EQUAL_UINT8(12, view::events(store, find(1, 10)).count);
}

void test_changing_a_single_target_preserves_other_competitions() {
  for (uint8_t c = 1; c <= cfg::kCompetitionMax; ++c) {
    add(c, 100 + c, false);
    TEST_ASSERT_NOT_NULL(store.ensureDetail(c, kProvider, 100 + c, {kNow, 1}));
  }
  add(1, 999, false, 0);
  TEST_ASSERT_NOT_NULL(store.ensureDetail(1, kProvider, 999, {kNow, 1}));
  TEST_ASSERT_FALSE(store.hasDetail(find(1, 101)));
  TEST_ASSERT_TRUE(store.hasDetail(find(1, 999)));
  for (uint8_t c = 2; c <= cfg::kCompetitionMax; ++c)
    TEST_ASSERT_TRUE(store.hasDetail(find(c, 100 + c)));
}

void test_ordinary_targets_cannot_displace_eight_own_detail_slots() {
  for (uint8_t c = 1; c <= cfg::kCompetitionMax; ++c) {
    add(c, 100 + c, true);
    add(c, 200 + c, false);
    TEST_ASSERT_NOT_NULL(store.ensureDetail(c, kProvider, 200 + c, {kNow, 1}));
  }
  for (uint8_t c = 1; c <= cfg::kCompetitionMax; ++c) {
    const auto& own = find(c, 100 + c);
    const auto& ordinary = find(c, 200 + c);
    TEST_ASSERT_TRUE(store.hasDetail(own));
    TEST_ASSERT_TRUE(own.detail < cfg::kStoreOwnMatches);
    TEST_ASSERT_TRUE(store.hasDetail(ordinary));
    TEST_ASSERT_TRUE(ordinary.detail >= cfg::kStoreOwnMatches);
  }
}

void test_both_event_banks_fill_without_taking_each_others_capacity() {
  for (uint8_t c = 1; c <= 4; ++c) {
    add(c, 100 + c, true, kNow - c * 3600);
    fillEvents(c, 100 + c, c * 100);
    add(c, 200 + c, false, kNow - c * 3600);
    TEST_ASSERT_NOT_NULL(store.ensureDetail(c, kProvider, 200 + c, {kNow, 1}));
    fillEvents(c, 200 + c, c * 1000);
  }
  unsigned used_own = 0, used_ordinary = 0;
  for (uint8_t i = 0; i < cfg::kStoreEvents; ++i) {
    if (store.eventAt(i).slot == kNoIndex) continue;
    if (i < cfg::kStoreOwnEvents)
      ++used_own;
    else
      ++used_ordinary;
  }
  TEST_ASSERT_EQUAL_UINT(48, used_own);
  TEST_ASSERT_EQUAL_UINT(48, used_ordinary);
  add(5, 205, false);
  TEST_ASSERT_NOT_NULL(store.ensureDetail(5, kProvider, 205, {kNow, 1}));
  fillEvents(5, 205, 5000);
  for (uint8_t c = 1; c <= 4; ++c) {
    const auto& own = find(c, 100 + c);
    TEST_ASSERT_FALSE(own.flags & kMatchEventsPoolCut);
    TEST_ASSERT_EQUAL_UINT8(12, view::events(store, own).count);
  }
  TEST_ASSERT_TRUE(find(4, 204).flags & kMatchEventsPoolCut);
  TEST_ASSERT_TRUE(store.notices(4) & kNoticeEvents);
}

void test_recorded_espn_ordinary_and_own_details_use_the_same_parser() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, recorded::kEspnDay));
  const auto row = doc["events"][0].as<JsonVariantConst>();
  MapContext c;
  c.competition = 1;
  c.provider = kProvider;
  c.club = 598;
  c.clock = {kNow, 1};
  TEST_ASSERT_TRUE(storeMatch(row, store, c));
  const auto& own = find(1, 401884775);
  const auto own_events = view::events(store, own);
  TEST_ASSERT_TRUE(own.isOwn());
  TEST_ASSERT_EQUAL_UINT8(4, own_events.count);
  const StoredEvent first = store.eventAt(own_events.index[0]);
  Text<cfg::kNameBytes> name;
  name.assign(view::teamName(store, own, true));
  store.clear();
  c.club = 0;
  TEST_ASSERT_TRUE(storeMatch(row, store, c));
  TEST_ASSERT_NOT_NULL(store.ensureDetail(1, kProvider, 401884775, c.clock));
  TEST_ASSERT_TRUE(storeMatch(row, store, c));
  const auto& ordinary = find(1, 401884775);
  const auto ordinary_events = view::events(store, ordinary);
  TEST_ASSERT_FALSE(ordinary.isOwn());
  TEST_ASSERT_EQUAL_UINT8(own_events.count, ordinary_events.count);
  TEST_ASSERT_EQUAL_STRING(name.data, view::teamName(store, ordinary, true));
  const auto& ordinary_first = store.eventAt(ordinary_events.index[0]);
  TEST_ASSERT_EQUAL_UINT32(first.identity, ordinary_first.identity);
  TEST_ASSERT_EQUAL_STRING(first.player.data, ordinary_first.player.data);
}

void test_live_observation_is_confirmed_and_does_not_slide_each_poll() {
  StoredMatch m;
  m.id = 10;
  m.competition = 1;
  m.provider = kProvider;
  m.phase = Phase::kLive;
  m.estimated = kEstimatedPhase;
  TEST_ASSERT_EQUAL_UINT32(0,
                           store.upsert(m, {kNow, 1})->first_live_observed_utc);
  m.estimated = 0;
  TEST_ASSERT_EQUAL_UINT32(kNow,
                           store.upsert(m, {kNow, 2})->first_live_observed_utc);
  TEST_ASSERT_EQUAL_UINT32(
      kNow, store.upsert(m, {kNow + 3600, 3})->first_live_observed_utc);
  m.kickoff = kNow - 600;
  TEST_ASSERT_EQUAL_UINT32(
      kNow, store.upsert(m, {kNow + 3601, 4})->first_live_observed_utc);
  TEST_ASSERT_EQUAL_UINT32(kNow - 600, find(1, 10).kickoff);
}

void test_suspension_keeps_observation_and_freezes_an_existing_minute() {
  StoredMatch m;
  m.id = 10;
  m.competition = 1;
  m.provider = kProvider;
  m.kickoff = kNow - 600;
  m.phase = Phase::kLive;
  m.minute = 10;
  store.upsert(m, {kNow, 1});
  m.phase = Phase::kSuspended;
  m.minute = kNoValue;
  store.upsert(m, {kNow + 60, 2});
  store.estimateAll(kNow + 7200);
  TEST_ASSERT_EQUAL(Phase::kSuspended, find(1, 10).phase);
  TEST_ASSERT_EQUAL_UINT8(10, find(1, 10).minute);
  TEST_ASSERT_EQUAL_UINT32(kNow, find(1, 10).first_live_observed_utc);
  TEST_ASSERT_EQUAL_UINT32(0, find(1, 10).finished_at);
  m.phase = Phase::kFinished;
  store.upsert(m, {kNow + 7200, 3});
  TEST_ASSERT_EQUAL_UINT32(kNow + 7200, find(1, 10).finished_at);
  TEST_ASSERT_EQUAL(Phase::kSuspended,
                    football::test_support::mappedStatus(
                        cfg::Provider::kApiFootball, "SUSP"));
  TEST_ASSERT_EQUAL(Phase::kSuspended,
                    football::test_support::mappedStatus(
                        cfg::Provider::kFootballData, "SUSPENDED"));
  TEST_ASSERT_EQUAL(Phase::kPostponed,
                    football::test_support::mappedStatus(
                        cfg::Provider::kFootballData, "POSTPONED"));
}

int32_t berlinFallOffset(int64_t instant, void*) {
  return instant < parseUtc("2026-10-25T01:00:00Z") ? 7200 : 3600;
}

void test_finish_day_retention_survives_midnight_dst_and_store_expiry() {
  StoredMatch m;
  m.id = 10;
  m.competition = 1;
  m.provider = kProvider;
  m.phase = Phase::kFinished;
  m.kickoff = parseUtc("2026-10-24T20:30:00Z");      // Saturday local
  m.finished_at = parseUtc("2026-10-24T22:30:00Z");  // Sunday local
  m.estimated = kEstimatedFinish;
  store.upsert(m, {kNow, 1});
  const DateContext dates{berlinFallOffset};
  const auto monday = parseUtc("2026-10-26T22:30:00Z");
  TEST_ASSERT_TRUE(resultHeld(find(1, 10), monday, dates));
  store.retain(1, retention::Selection{}, monday, dates);
  TEST_ASSERT_NOT_NULL(store.find(1, kProvider, 10));
  TEST_ASSERT_TRUE(find(1, 10).estimated & kEstimatedFinish);
  const auto tuesday = parseUtc("2026-10-26T23:00:00Z");
  TEST_ASSERT_FALSE(resultHeld(find(1, 10), tuesday, dates));
  store.retain(1, retention::Selection{}, tuesday, dates);
  TEST_ASSERT_NULL(store.find(1, kProvider, 10));
}

void test_result_hold_recomputes_after_timezone_change() {
  StoredMatch m;
  m.phase = Phase::kFinished;
  m.finished_at = parseUtc("2026-10-18T23:30:00Z");
  const auto now = parseUtc("2026-10-20T00:00:00Z");
  TEST_ASSERT_FALSE(resultHeld(m, now, DateContext{}));
  TEST_ASSERT_TRUE(resultHeld(m, now, DateContext{nullptr, nullptr, 7200}));
}
}  // namespace

void setUp() { store.clear(); }
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ordinary_details_require_an_explicit_identity);
  RUN_TEST(test_changing_a_single_target_preserves_other_competitions);
  RUN_TEST(test_ordinary_targets_cannot_displace_eight_own_detail_slots);
  RUN_TEST(test_both_event_banks_fill_without_taking_each_others_capacity);
  RUN_TEST(test_recorded_espn_ordinary_and_own_details_use_the_same_parser);
  RUN_TEST(test_live_observation_is_confirmed_and_does_not_slide_each_poll);
  RUN_TEST(test_suspension_keeps_observation_and_freezes_an_existing_minute);
  RUN_TEST(test_finish_day_retention_survives_midnight_dst_and_store_expiry);
  RUN_TEST(test_result_hold_recomputes_after_timezone_change);
  return UNITY_END();
}
