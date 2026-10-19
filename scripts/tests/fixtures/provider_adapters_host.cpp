// Real public response replay plus reproduced replacement/partial failures.
// No transport/device claims and no JSON DOM in the tested production path.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <vector>

#include "provider_adapter.h"
#include "provider_plan.h"
#include "screen_selection.h"
#include "store_mappers.h"
#include "store_views.h"
namespace allocation_probe {
std::size_t calls = 0, bytes = 0;
bool fail = false;
void reset(bool failed = false) {
  calls = bytes = 0;
  fail = failed;
}
}  // namespace allocation_probe
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  ++allocation_probe::calls;
  allocation_probe::bytes += bytes;
  if (allocation_probe::fail) return nullptr;
  try {
    return ::operator new(bytes);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* bytes, const std::nothrow_t&) noexcept {
  ::operator delete(bytes);
}
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
    map.clock = {parseUtc("2026-10-17T14:00:00Z"), 100};
    assert(store.bind(1, 1, club, map.clock));
    SourceKey key;
    assert(key.assign(provider, competition, season));
    map.source = store.sourceSlot(store.resolve(1, key, map.clock));
    assert(map.source != kNoSource);
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
    if (request.operation == provider::Operation::kTable)
      store.beginTable(map.source);
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
  const auto* match = cup.store.find(cup.map.source, 401875180);
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
  settings::CompetitionRoutes routes;
  assert(routes.entries.push(entry));
  settings::screen_policy::Eligibility eligibility;
  planner.context(routes, eligibility);
  planner.synchronize(h.store, h.map.clock);
  job.key.competition = entry.id;
  job.key.incarnation = entry.incarnation;
  job.slot = cfg::kStoreSources;
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
  assert(planner.finish(true, h.map.clock) == plan::Step::kDone);
  // Resolution finishes the view scope; the shared-source fixture acquisition
  // remains pending and must begin with the first retained scoreboard day.
  schedule::Inputs demand;
  planner.prepareDemand(h.store, h.map.clock, demand);
  const auto source = h.store.currentSource(entry.id);
  assert(demand.scopes[source].key.source == h.store.sourceId(source));
  auto continuation = job;
  continuation.key = demand.scopes[source].key;
  continuation.slot = source;
  plan::Request fixtures;
  assert(planner.request(continuation, entry, h.store, h.map.clock, fixtures));
  assert(fixtures.spec.from == parseUtc("2026-10-09T07:00Z"));
  planner.release();
  const auto& state = planner.state(h.store.currentSource(entry.id));
  assert(state.day_count == 9);
  assert(state.days[0] == parseUtc("2026-10-09T07:00Z"));
  assert(state.days[8] == parseUtc("2026-10-25T07:00Z"));
  // A capacity-bound window never silently becomes a complete list.
  uint32_t output[8]{};
  bool complete = true;
  assert(plan::matchdayDays(state.days.data(), state.day_count, h.map.clock.utc,
                            output, 8, &complete) == 8 &&
         !complete);
  std::printf(
      "PASS real ESPN calendar %zuB/56 dates: previous/current/next complete "
      "blocks retained (9 days), overflow explicit\n",
      bytes.size());
}

void normalized_round_meaning() {
  for (const auto provider :
       {cfg::Provider::kApiFootball, cfg::Provider::kFootballData}) {
    Harness h(provider, "39", "2026");
    const auto body =
        provider == cfg::Provider::kApiFootball
            ? R"({"response":[{"fixture":{"id":1,"date":"2026-10-17T18:00:00Z"},"league":{"id":39,"season":2026,"round":"Quarterfinals"},"teams":{"home":{"id":5},"away":{"id":8}}}]})"
            : R"({"matches":[{"id":1,"utcDate":"2026-10-17T18:00:00Z","stage":"REGULAR_SEASON","homeTeam":{"id":5},"awayTeam":{"id":8}}]})";
    assert(h.parse(body).status == provider::Status::kOk);
    settings::CompetitionRoutes routes;
    routes.entries.count = 1;
    auto& route = routes.entries[0];
    route.id = 1;
    route.incarnation = 1;
    route.active = true;
    route.primary.provider = provider;
    route.primary.competition.assign("39");
    route.primary.season.assign("2026");
    const selection::Input input{settings::screen_policy::Options{}, routes,
                                 h.store, h.map.clock};
    const auto selected = selection::phasePeriod(input, route);
    assert(selected.valid());
    assert(selected.verified == (provider == cfg::Provider::kApiFootball));
    // Missing label in a later partial response does not erase its meaning.
    assert(h.parse(provider == cfg::Provider::kApiFootball
                       ? R"({"response":[{"fixture":{"id":1}}]})"
                       : R"({"matches":[{"id":1}]})")
               .status == provider::Status::kOk);
    assert(selection::phasePeriod(input, route).verified == selected.verified);
  }
  provider::SelectionPlan plan;
  for (unsigned i = 0; i < cfg::kProviderCount; ++i) {
    const auto p = static_cast<cfg::Provider>(i);
    const auto requirements = settings::providerRequirements(p);
    for (const auto operation :
         {provider::Operation::kCompetitions, provider::Operation::kTeams,
          provider::Operation::kOriginalNames}) {
      assert(provider::prepareSelection({p, "39", "2026"}, operation,
                                        parseUtc("2026-10-18T12:00:00Z"),
                                        plan));
      assert(!plan.discover && std::string_view(plan.season.data) == "2026");
      assert(provider::prepareSelection(
          {p, "39", ""}, operation, parseUtc("2026-10-18T12:00:00Z"), plan));
      assert(plan.discover ==
             (operation != provider::Operation::kCompetitions &&
              requirements.discover_teams));
    }
  }
  std::puts(
      "PASS actual AF round/FD stage normalization through selection, partial "
      "preservation and all-provider automatic/explicit setup");
}
void partial() {
  Harness h(cfg::Provider::kFootballData, "BL1", "2026", 5);
  const char* complete =
      R"({"matches":[{"id":1,"utcDate":"2026-10-17T13:30:00Z","status":"IN_PLAY","homeTeam":{"id":5,"name":"Bayern"},"awayTeam":{"id":8,"name":"Other"},"score":{"duration":"REGULAR","fullTime":{"home":2,"away":1}}}]})";
  assert(h.parse(complete).status == provider::Status::kOk);
  auto before = *h.store.find(h.map.source, 1);
  auto result = h.parse(
      R"({"matches":[{"id":1,"score":{"fullTime":{"home":null,"away":null}}},{"id":2}")");
  assert(result.status == provider::Status::kBadResponse &&
         !result.document_complete);
  const auto* match = h.store.find(h.map.source, 1);
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
         h.store.tableState(h.map.source) == DatasetState::kPartial);
  assert(h.parse("{}").status == provider::Status::kMissing);
  assert(h.store.rowCount() == 2);
  assert(h.parse(R"({"standings":[]})").status == provider::Status::kOk);
  assert(!h.store.rowCount() && !h.store.groupCount() &&
         h.store.tableState(h.map.source) == DatasetState::kComplete);
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
  auto* match = h.store.find(h.map.source, 1);
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
      assert(h.store.team(h.map.source, id, "Original", "ORG") != kNoTeam);
    StoredMatch old;
    old.id = 1;
    old.source = h.map.source;
    old.home = 0;
    old.away = 1;
    old.principal = {2, 1};
    old.phase = Phase::kLive;
    h.store.bind(1, 1, 1, h.map.clock);
    assert(h.store.upsert(old, h.map.clock));
    const auto result = h.parse(body);
    assert(result.status == provider::Status::kRejected &&
           !result.document_complete);
    const auto* kept = h.store.find(h.map.source, 1);
    assert(kept && kept->home == old.home && kept->away == old.away &&
           kept->principal == old.principal && kept->phase == old.phase);
    assert(h.store.datasetNotice(h.map.source, MatchStore::Dataset::kFixtures,
                                 h.map.clock.utc) == Cause::kStoreFull);
    assert(h.store.teamCount() == 256 &&
           std::string_view(view::teamName(h.store, *kept, false)) ==
               "Original");
    h.store.bind(1, 1, 2, h.map.clock);
    kept = h.store.find(h.map.source, 1);
    assert(kept && h.store.isOwn(*kept) && kept->principal == old.principal &&
           std::string_view(view::teamName(h.store, *kept, false)) ==
               "Original");
  }
  std::puts(
      "PASS all4 actual parsers: full256team directory rejects corrected "
      "opponent and score together; original labels/facts survive "
      "reclassification with E19");
}
void workspace_is_allocated_only_for_required_staging() {
  for (bool fail : {false, true}) {
    Harness metadata(cfg::Provider::kOpenLigaDb, "bl1", "2026");
    metadata.request.operation = provider::Operation::kLastChange;
    allocation_probe::reset(fail);
    auto parsed = metadata.parse(R"("2026-10-18T10:00:00Z")");
    assert(parsed.status == provider::Status::kOk &&
           allocation_probe::calls == 0);
    Harness empty(cfg::Provider::kOpenLigaDb, "bl1", "2026");
    allocation_probe::reset(fail);
    parsed = empty.parse("[]");
    assert(parsed.status == provider::Status::kOk &&
           allocation_probe::calls == 0);
    empty.request.operation = provider::Operation::kTable;
    allocation_probe::reset(fail);
    parsed = empty.parse("[]");
    assert(parsed.status == provider::Status::kOk &&
           empty.store.tableState(empty.map.source) ==
               DatasetState::kComplete &&
           allocation_probe::calls == 0);
  }
  Harness events(cfg::Provider::kOpenLigaDb, "bl1", "2026");
  const auto body =
      R"([{"matchID":1,"team1":{"teamId":1},"team2":{"teamId":2},"goals":[{"goalID":1,"matchMinute":1,"scoreTeam1":1,"scoreTeam2":0}]}])";
  allocation_probe::reset(true);
  auto parsed = events.parse(body);
  assert(parsed.status == provider::Status::kNoMemory &&
         !parsed.document_complete && events.store.matchCount() == 0 &&
         allocation_probe::calls == 1);
  allocation_probe::reset();
  parsed = events.parse(body);
  assert(parsed.status == provider::Status::kOk && events.events == 1 &&
         allocation_probe::calls == 1 &&
         allocation_probe::bytes ==
             sizeof(provider::Event) * cfg::kStoreEvents);
  const std::pair<cfg::Provider, const char*> tables[] = {
      {cfg::Provider::kFootballData,
       R"({"standings":[{"type":"TOTAL","table":[{"team":{"id":1},"position":1,"playedGames":1,"points":3,"goalDifference":1}]}]})"},
      {cfg::Provider::kApiFootball,
       R"({"response":[{"league":{"standings":[[{"rank":1,"team":{"id":1},"points":3,"goalsDiff":1,"all":{"played":1}}]]}}]})"}};
  for (const auto& [provider, body] : tables) {
    Harness table(provider, "league", "2026");
    table.request.operation = provider::Operation::kTable;
    table.store.beginTable(table.map.source);
    StoredRow old;
    old.team = table.store.team(table.map.source, 1, "Old club");
    old.points = 13;
    assert(table.store.appendRow(old) && table.store.commitTable(true));
    allocation_probe::reset(true);
    auto result = table.parse(body);
    assert(result.status == provider::Status::kNoMemory &&
           !result.document_complete && allocation_probe::calls == 1 &&
           table.store.rowCount() == 1 && table.store.row(0).points == 13);
    allocation_probe::reset();
    result = table.parse(body);
    assert(result.status == provider::Status::kOk &&
           allocation_probe::calls == 1 && table.store.rowCount() == 1 &&
           table.store.row(0).points == 3);
  }
  std::puts(
      "PASS actual parser: metadata/empty table survives denied workspace; "
      "first required event has exact one OOM; retry allocates one bounded "
      "payload");
}
}  // namespace
int main(int argc, char** argv) {
  assert(argc == 2);
  workspace_is_allocated_only_for_required_staging();
  actual(argv[1]);
  calendar(argv[1]);
  normalized_round_meaning();
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
