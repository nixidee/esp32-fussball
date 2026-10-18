// Real public response replay plus reproduced replacement/partial failures.
// No transport/device claims and no JSON DOM in the tested production path.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "provider_adapter.h"
#include "provider_plan.h"
#include "store_mappers.h"
#include "store_views.h"
using namespace football;
namespace football::provider {
// This focused sporting replay intentionally excludes interactive selection.
ParseResult readSelectionResponse(json::Reader&, const RequestSpec&,
                                  const Sink&) {
  return {};
}
}  // namespace football::provider
namespace {
struct Body {
  std::string_view bytes;
  std::size_t at = 0;
  static int read(void* raw) {
    auto& self = *static_cast<Body*>(raw);
    return self.at < self.bytes.size()
               ? static_cast<unsigned char>(self.bytes[self.at++])
               : -1;
  }
};
struct Harness {
  MatchStore store;
  MapContext map;
  provider::RequestSpec request;
  unsigned fixtures = 0, events = 0, rows = 0, groups = 0;
  std::vector<uint32_t> shot_ids;
  Harness(cfg::Provider provider, const char* competition, const char* season,
          uint32_t club = 0) {
    request.source = {provider, competition, season};
    map.provider = provider;
    map.competition = 1;
    map.club = club;
    map.clock = {parseUtc("2026-10-17T14:00:00Z"), 100};
  }
  provider::ParseResult parse(std::string_view bytes) {
    Body input{bytes};
    json::Reader reader({&input, Body::read});
    provider::Sink sink;
    sink.context = this;
    sink.fixture = [](const provider::Fixture& fixture,
                      std::span<const provider::Event> events, void* raw) {
      auto& h = *static_cast<Harness*>(raw);
      ++h.fixtures;
      h.events += events.size();
      for (const auto& e : events)
        if (e.facts.flags & kEventShootout)
          h.shot_ids.push_back(e.facts.identity);
      return storeMatch(fixture, events, h.store, h.map);
    };
    sink.group = [](const provider::TableGroup& group, void* raw) {
      auto& h = *static_cast<Harness*>(raw);
      ++h.groups;
      return h.store.appendGroup(group.key.data, group.label.data,
                                 group.ordinal);
    };
    sink.row = [](const provider::TableRow& row, void* raw) {
      auto& h = *static_cast<Harness*>(raw);
      ++h.rows;
      return storeTableRow(row, h.store, h.map);
    };
    if (request.operation == provider::Operation::kTable) store.beginTable(1);
    auto result = provider::readResponse(reader, request, sink);
    if (request.operation == provider::Operation::kTable)
      store.commitTable(result.status == provider::Status::kOk &&
                        result.document_complete && result.primary_seen);
    if (result.status == provider::Status::kOk)
      assert(input.at == bytes.size());
    return result;
  }
};
std::string file(const char* root, const char* name) {
  std::ifstream in(std::string(root) + "/" + name);
  assert(in.good());
  return {std::istreambuf_iterator<char>(in), {}};
}
void actual(const char* root) {
  Harness olg(cfg::Provider::kOpenLigaDb, "bl1", "2026", 40);
  auto season = file(root, "openliga-season.json");
  assert(season.size() == 299845);
  auto result = olg.parse(season);
  assert(result.status == provider::Status::kOk && result.document_complete &&
         result.metadata.metadata_complete);
  assert(olg.fixtures == 306 && olg.events == 173 &&
         olg.store.matchCount() == 306);
  Harness espn(cfg::Provider::kEspn, "ger.1", "2026", 132);
  result = espn.parse(file(root, "espn-scoreboard.json"));
  assert(result.status == provider::Status::kOk && espn.fixtures > 0);
  Harness cup(cfg::Provider::kEspn, "ger.dfb_pokal", "2026", 269);
  result = cup.parse(file(root, "espn-cup.json"));
  assert(result.status == provider::Status::kOk);
  const auto* match = cup.store.find(1, cfg::Provider::kEspn, 401875180);
  assert(match);
  assert(match->period == Period::kShootout);
  const auto attempts = view::shootout(cup.store, *match);
  assert(attempts.count == 9);
  for (unsigned i = 0; i < attempts.count; ++i) {
    const auto& event = cup.store.eventAt(attempts.index[i]);
    assert(event.ordinal == i);
    assert(event.type == EventType::kPenaltyGoal);
    for (unsigned j = 0; j < i; ++j)
      assert(event.identity != cup.store.eventAt(attempts.index[j]).identity);
  }
  // The public scoreboard reports scored attempts only. No missed attempts
  // are synthesized from the final5:4 total or the reporting gaps.
  assert(match->shootout.home == 4 && match->shootout.away == 5);
  std::printf(
      "PASS real OLDB %zuB/306 fixtures/173 events; ESPN league/cup including9 "
      "reported sudden-death goals\n",
      season.size());
}
void calendar(const char* root) {
  Harness h(cfg::Provider::kEspn, "ger.1", "2026");
  plan::Planner planner;
  settings::CompetitionRoute entry;
  entry.id = 1;
  entry.incarnation = 1;
  entry.primary.provider = cfg::Provider::kEspn;
  entry.primary.competition.assign("ger.1");
  schedule::Job job;
  job.slot = 0;
  job.competition = 1;
  job.provider = cfg::Provider::kEspn;
  job.type = cfg::DataType::kFixtures;
  plan::Request request;
  assert(planner.request(job, entry, h.store, h.map.clock, request));
  auto bytes = file(root, "espn-calendar.json");
  Body input{bytes};
  json::Reader reader({&input, Body::read});
  provider::Sink sink;
  sink.context = &planner;
  sink.day = [](uint32_t at, void* raw) {
    return static_cast<plan::Planner*>(raw)->consumeDay(at);
  };
  sink.calendar_round = [](const provider::CalendarRound& round, void* raw) {
    return static_cast<plan::Planner*>(raw)->consumeCalendarRound(round);
  };
  const auto parsed = provider::readResponse(reader, request.spec, sink);
  assert(parsed.status == provider::Status::kOk && parsed.primary_seen &&
         parsed.rows == 56);
  assert(planner.response(parsed));
  assert(planner.finish(true, h.map.clock) == plan::Step::kMore);
  const auto& state = planner.state(0);
  assert(state.fixtures.count == 9);
  assert(state.fixtures.calendar_data.days[0] == parseUtc("2026-10-09T07:00Z"));
  assert(state.fixtures.calendar_data.days[8] == parseUtc("2026-10-25T07:00Z"));
  // A capacity-bound window never silently becomes a complete list.
  uint32_t output[8]{};
  bool complete = true;
  assert(plan::matchdayDays(state.fixtures.calendar_data.days.data(),
                            state.fixtures.count, h.map.clock.utc, output, 8,
                            &complete) == 8 &&
         !complete);
  std::printf(
      "PASS real ESPN calendar %zuB/56 dates: previous/current/next complete "
      "blocks retained (9 days), overflow explicit\n",
      bytes.size());
}

void partial() {
  Harness h(cfg::Provider::kFootballData, "BL1", "2026", 5);
  const char* complete =
      R"({"matches":[{"id":1,"utcDate":"2026-10-17T13:30:00Z","status":"IN_PLAY","homeTeam":{"id":5,"name":"Bayern"},"awayTeam":{"id":8,"name":"Other"},"score":{"duration":"REGULAR","fullTime":{"home":2,"away":1}}}]})";
  assert(h.parse(complete).status == provider::Status::kOk);
  auto before = *h.store.find(1, h.map.provider, 1);
  auto result = h.parse(
      R"({"matches":[{"id":1,"score":{"fullTime":{"home":null,"away":null}}},{"id":2}")");
  assert(result.status == provider::Status::kBadResponse &&
         !result.document_complete);
  const auto* match = h.store.find(1, h.map.provider, 1);
  assert(match && match->principal == before.principal &&
         match->home == before.home && match->kickoff == before.kickoff &&
         match->phase == before.phase);
  result = h.parse(R"({"matches":[],"matches":[]})");
  assert(result.status == provider::Status::kBadResponse &&
         !result.document_complete);
  result = h.parse(R"({"events":[{"id":1}]})");
  assert(result.status == provider::Status::kMissing);
  result = h.parse(R"({"matches":[{"id":"4294967296"}]})");
  assert(result.status == provider::Status::kBadResponse);
  std::puts(
      "PASS missing fields/invalid body preserve known score,time,team,phase; "
      "duplicate and wrong primary rejected");
}
void tables() {
  Harness h(cfg::Provider::kFootballData, "BL1", "2026");
  h.request.operation = provider::Operation::kTable;
  const char* body =
      R"({"standings":[{"table":[{"position":1,"team":{"id":1,"name":"A"},"playedGames":3,"points":9}],"group":"A","stage":"GROUP_STAGE","type":"TOTAL"},{"type":"TOTAL","group":"B","stage":"GROUP_STAGE","table":[{"position":1,"team":{"id":2,"name":"B"},"playedGames":3,"points":7}]}]})";
  assert(h.parse(body).status == provider::Status::kOk);
  assert(h.store.rowCount() == 2 && h.store.groupCount() == 2);
  assert(h.store.row(0).group == 0 && h.store.row(1).group == 1);
  assert(std::string_view(h.store.labelAt(h.store.group(0).label).data) == "A");
  auto old = h.store.row(0);
  assert(
      h.parse(
           R"({"standings":[{"type":"TOTAL","table":[{"position":8,"team":{"id":9}}])")
          .status == provider::Status::kBadResponse);
  assert(h.store.rowCount() == 2 && h.store.row(0).team == old.team &&
         h.store.groupCount() == 2 &&
         h.store.tableState(1) == DatasetState::kPartial);
  assert(h.parse("{}").status == provider::Status::kMissing);
  assert(h.store.rowCount() == 2);
  assert(h.parse(R"({"standings":[]})").status == provider::Status::kOk);
  assert(!h.store.rowCount() && !h.store.groupCount() &&
         h.store.tableState(1) == DatasetState::kComplete);
  Harness af(cfg::Provider::kApiFootball, "39", "2026");
  af.request.operation = provider::Operation::kTable;
  assert(
      af.parse(
            R"({"response":[{"league":{"standings":[[{"rank":1,"team":{"id":1,"name":"A"},"points":5}],[{"rank":1,"team":{"id":2,"name":"B"},"points":4}]]}}]})")
          .status == provider::Status::kOk);
  assert(af.store.groupCount() == 2 && af.store.rowCount() == 2 &&
         af.store.row(1).group == 1);
  std::puts(
      "PASS FD unordered TOTAL/group table and AF multiple groups; "
      "failed/missing keeps old, complete empty clears");
}
void attempts() {
  Harness h(cfg::Provider::kEspn, "cup", "2026", 1);
  auto body = [](bool correct) {
    std::string s =
        R"({"events":[{"id":1,"competitions":[{"competitors":[{"homeAway":"home","team":{"id":1}},{"homeAway":"away","team":{"id":2}}],"details":[)";
    for (unsigned i = 0; i < 18; ++i) {
      if (i) s += ',';
      s +=
          R"({"type":{"text":"Penalty - Scored"},"penaltyKick":true,"shootout":true,"scoringPlay":)";
      s += (i == 0 && correct ? "false" : "true");
      s += R"(,"clock":{"displayValue":"120'"},"team":{"id":1}})";
    }
    return s + "]}]}]}";
  };
  assert(h.parse(body(false)).status == provider::Status::kOk);
  auto* match = h.store.find(1, h.map.provider, 1);
  assert(match);
  auto shots = view::shootout(h.store, *match);
  assert(shots.count == 18);
  auto first = h.store.eventAt(shots.index[0]).identity;
  assert(h.parse(body(true)).status == provider::Status::kOk);
  shots = view::shootout(h.store, *match);
  assert(shots.count == 18 &&
         h.store.eventAt(shots.index[0]).identity == first &&
         h.store.eventAt(shots.index[0]).type == EventType::kPenaltyMissed);
  assert(h.store.detailAt(match->detail).event_count == 0);
  std::puts(
      "PASS18 same-minute anonymous attempts remain distinct; scored→missed "
      "correction keeps attempt identity");
}
void event_scope_is_per_fixture() {
  for (unsigned count : {96u, 97u}) {
    std::string body =
        R"({"events":[{"id":0},{"id":1,"competitions":[{"details":[)";
    for (unsigned i = 0; i < count; ++i) {
      if (i) body += ',';
      body +=
          R"({"type":{"text":"Penalty - Scored"},"penaltyKick":true,"shootout":true,"scoringPlay":true,"clock":{"displayValue":"120'"}})";
    }
    body += "]}]}]}";
    struct Observed {
      unsigned rows = 0, events = 0;
      bool cut = false;
    } observed;
    provider::RequestSpec spec;
    spec.source = {cfg::Provider::kEspn, "cup", "2026"};
    provider::Sink sink;
    sink.context = &observed;
    sink.fixture = [](const provider::Fixture& fixture,
                      std::span<const provider::Event> events, void* raw) {
      auto& out = *static_cast<Observed*>(raw);
      ++out.rows;
      out.events = events.size();
      out.cut = fixture.events_truncated;
      return true;
    };
    Body input{body};
    json::Reader reader({&input, Body::read});
    const auto parsed = provider::readResponse(reader, spec, sink);
    assert(parsed.status == provider::Status::kBadResponse &&
           observed.rows == 1 && observed.events == 96);
    assert(observed.cut == (count > 96));
  }
  std::puts(
      "PASS bad prior fixture does not mark exactly96 events truncated;97th "
      "actual event sets cut");
}
void full_team_directory_rejects_a_changed_opponent_with_its_score() {
  const std::pair<cfg::Provider, const char*> cases[] = {
      {cfg::Provider::kOpenLigaDb,
       R"([{"matchID":1,"team1":{"teamId":1},"team2":{"teamId":999},"matchResults":[{"resultTypeID":2,"pointsTeam1":9,"pointsTeam2":8}]}])"},
      {cfg::Provider::kEspn,
       R"({"events":[{"id":1,"competitions":[{"status":{"type":{"state":"post","completed":true}},"competitors":[{"homeAway":"home","team":{"id":1},"score":9},{"homeAway":"away","team":{"id":999},"score":8}]}]}]})"},
      {cfg::Provider::kApiFootball,
       R"({"response":[{"fixture":{"id":1},"teams":{"home":{"id":1},"away":{"id":999}},"goals":{"home":9,"away":8}}]})"},
      {cfg::Provider::kFootballData,
       R"({"matches":[{"id":1,"homeTeam":{"id":1},"awayTeam":{"id":999},"score":{"duration":"REGULAR","fullTime":{"home":9,"away":8}}}]})"}};
  for (const auto& [provider, body] : cases) {
    Harness h(provider, "league", "2026", 1);
    for (uint32_t id = 1; id <= cfg::kStoreTeams; ++id)
      assert(h.store.team(1, provider, id, "Original", "ORG") != kNoTeam);
    StoredMatch old;
    old.id = 1;
    old.competition = 1;
    old.provider = provider;
    old.home = 0;
    old.away = 1;
    old.principal = {2, 1};
    old.phase = Phase::kLive;
    old.flags = kMatchOwn;
    assert(h.store.upsert(old, h.map.clock));
    const auto result = h.parse(body);
    assert(result.status == provider::Status::kRejected &&
           !result.document_complete);
    const auto* kept = h.store.find(1, provider, 1);
    assert(kept && kept->home == old.home && kept->away == old.away &&
           kept->principal == old.principal && kept->phase == old.phase);
    assert(h.store.datasetNotice(1, MatchStore::Dataset::kFixtures,
                                 h.map.clock.utc) == Cause::kStoreFull);
    assert(h.store.teamCount() == 256 &&
           std::string_view(view::teamName(h.store, *kept, false)) ==
               "Original");
    h.store.selectClub(1, provider, 2, h.map.clock);
    kept = h.store.find(1, provider, 1);
    assert(kept && kept->isOwn() && kept->principal == old.principal &&
           std::string_view(view::teamName(h.store, *kept, false)) ==
               "Original");
  }
  std::puts(
      "PASS all4 actual parsers: full256team directory rejects corrected "
      "opponent and score together; original labels/facts survive "
      "reclassification with E19");
}
}  // namespace
int main(int argc, char** argv) {
  assert(argc == 2);
  actual(argv[1]);
  calendar(argv[1]);
  partial();
  tables();
  attempts();
  event_scope_is_per_fixture();
  full_team_directory_rejects_a_changed_opponent_with_its_score();
  std::printf(
      "sizes Store=%zu Fixture=%zu EventInput=%zu Round=%zu Group=%zu "
      "Row=%zu\n",
      sizeof(MatchStore), sizeof(provider::Fixture), sizeof(provider::Event),
      sizeof(StoredRound), sizeof(StoredTableGroup), sizeof(StoredRow));
}
