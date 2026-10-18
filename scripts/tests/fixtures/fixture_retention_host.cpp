#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "fixture_retention.h"
#include "provider_plan.h"
#include "store_mappers.h"

namespace allocation_probe {
bool deny_index = false;
unsigned denied = 0;
}  // namespace allocation_probe
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  if (allocation_probe::deny_index &&
      bytes == sizeof(football::retention::Index)) {
    ++allocation_probe::denied;
    return nullptr;
  }
  try {
    return ::operator new(bytes);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  ::operator delete(pointer);
}

namespace football::provider {
ParseResult readSelectionResponse(json::Reader&, const RequestSpec&,
                                  const Sink&) {
  return {};
}
}  // namespace football::provider
struct Input {
  std::string_view text;
  std::size_t at = 0;
  static int read(void* raw) {
    auto& in = *static_cast<Input*>(raw);
    return in.at < in.text.size() ? static_cast<unsigned char>(in.text[in.at++])
                                  : -1;
  }
};
std::string load(const char* path) {
  std::ifstream in(path);
  assert(in);
  return {std::istreambuf_iterator<char>(in), {}};
}
int main(int argc, char** argv) {
  using namespace football;
  assert(argc >= 9);
  const auto utc = parseUtc("2026-10-14T12:00:00Z");
  provider::RequestSpec request;
  request.source = {cfg::Provider::kOpenLigaDb, "bl1", "2026"};
  retention::Selection previous;
  for (int pass = 1; pass <= 2; ++pass) {
    const auto body = load(argv[pass]);
    retention::Index index(40, utc);
    Input input{body};
    json::Reader reader({&input, Input::read});
    provider::Sink sink;
    sink.context = &index;
    sink.fixture = [](const provider::Fixture& row,
                      std::span<const provider::Event>, void* raw) {
      return static_cast<retention::Index*>(raw)->observe(row);
    };
    const auto parsed = provider::readResponse(reader, request, sink);
    assert(parsed.status == provider::Status::kOk && parsed.document_complete &&
           parsed.rows == 306);
    retention::Selection selected;
    assert(index.select(selected));
    if (pass == 1)
      previous = selected;
    else {
      assert(selected.count == previous.count && selected.own == previous.own);
      for (uint8_t i = 0; i < selected.count; ++i)
        assert(selected.rounds[i] == previous.rounds[i]);
    }
    MatchStore store;
    MapContext map;
    map.competition = 1;
    map.provider = request.source.provider;
    map.club = 40;
    map.clock = {utc, 100};
    struct Target {
      MatchStore& store;
      MapContext& map;
      const retention::Selection& selected;
    } target{store, map, selected};
    Input again{body};
    json::Reader second({&again, Input::read});
    sink.context = &target;
    sink.fixture = [](const provider::Fixture& row,
                      std::span<const provider::Event> events, void* raw) {
      auto& to = *static_cast<Target*>(raw);
      if (!to.selected.contains(row.facts.round, row.round_label.data,
                                row.facts.kickoff, row.facts.id))
        return true;
      return storeMatch(row, events, to.store, to.map);
    };
    assert(provider::readResponse(second, request, sink).status ==
           provider::Status::kOk);
    std::set<uint32_t> expected;
    for (int arg = 8; arg < argc; ++arg)
      expected.insert(static_cast<uint32_t>(std::stoul(argv[arg])));
    assert(store.matchCount() == expected.size());
    for (std::size_t i = 0; i < store.matchCount(); ++i)
      assert(expected.erase(store.match(i).id) == 1);
    assert(expected.empty() && store.teamCount() == 18 &&
           !(store.notices(1) & kNoticeMatches));
  }
  // One actual season index serves all eight configured clubs. Expected
  // identities are independently selected from the recorded response in Python.
  std::ifstream oracle(argv[7]);
  const auto read_list = [&]() {
    unsigned count = 0;
    assert(oracle >> count);
    std::vector<uint32_t> result(count);
    for (auto& value : result) assert(oracle >> value);
    return result;
  };
  auto clubs = read_list();
  const auto expected_own = read_list(), expected_union = read_list();
  assert(clubs.size() == cfg::kCompetitionMax);
  for (int pass = 1; pass <= 2; ++pass) {
    const auto body = load(argv[pass]);
    // A repeated preference must not consume another club or match slot.
    clubs.push_back(clubs.front());
    retention::Index index(clubs, utc);
    clubs.pop_back();
    Input input{body};
    json::Reader reader({&input, Input::read});
    provider::Sink sink;
    sink.context = &index;
    sink.fixture = [](const provider::Fixture& row,
                      std::span<const provider::Event>, void* raw) {
      return static_cast<retention::Index*>(raw)->observe(row);
    };
    assert(provider::readResponse(reader, request, sink).status ==
           provider::Status::kOk);
    retention::Selection selection;
    assert(index.select(selection));
    std::set<uint32_t> own_ids;
    for (const auto id : selection.own)
      if (id) assert(own_ids.insert(id).second);
    assert(own_ids ==
           std::set<uint32_t>(expected_own.begin(), expected_own.end()));
    assert(own_ids.size() > cfg::kRetainedOwnFixtures);
    MatchStore merged;
    MapContext map;
    map.competition = 1;
    map.provider = request.source.provider;
    map.clock = {utc, 100};
    struct Admission {
      retention::Selection& selection;
      MatchStore& store;
      MapContext& map;
    } admission{selection, merged, map};
    Input second_input{body};
    json::Reader second({&second_input, Input::read});
    sink.context = &admission;
    sink.fixture = [](const provider::Fixture& row,
                      std::span<const provider::Event> events, void* raw) {
      auto& target = *static_cast<Admission*>(raw);
      return !target.selection.contains(row.facts.round, row.round_label.data,
                                        row.facts.kickoff, row.facts.id) ||
             storeMatch(row, events, target.store, target.map);
    };
    assert(provider::readResponse(second, request, sink).status ==
           provider::Status::kOk);
    std::set<uint32_t> actual;
    for (std::size_t i = 0; i < merged.matchCount(); ++i)
      assert(actual.insert(merged.match(i).id).second);
    assert(actual ==
           std::set<uint32_t>(expected_union.begin(), expected_union.end()));
  }
  // Drive the production Planner with eight independent configured clubs of
  // the same complete-season source. Its final scope must account for every
  // selected own identity, including indices beyond the old byte mask.
  settings::CompetitionRoutes shared_routes;
  for (std::size_t i = 0; i < clubs.size(); ++i) {
    settings::CompetitionRoute entry;
    entry.id = static_cast<uint8_t>(i + 1);
    entry.incarnation = entry.id;
    entry.primary.provider = cfg::Provider::kOpenLigaDb;
    assert(entry.primary.competition.assign("bl1"));
    assert(entry.primary.season.assign("2026"));
    entry.club.team = clubs[i];
    assert(shared_routes.entries.push(entry));
  }
  settings::screen_policy::Eligibility eligibility;
  plan::Planner shared_planner;
  shared_planner.context(shared_routes, eligibility);
  MatchStore shared_store;
  schedule::Job shared_job;
  shared_job.competition = 1;
  shared_job.type = cfg::DataType::kFixtures;
  const auto shared_body = load(argv[1]);
  const auto shared_request = [&](uint32_t omit) {
    plan::Request next;
    assert(shared_planner.request(shared_job, shared_routes.entries[0],
                                  shared_store, {utc, 100}, next));
    Input input{shared_body};
    json::Reader reader({&input, Input::read});
    struct Receive {
      plan::Planner& planner;
      uint32_t omit;
    } receive{shared_planner, omit};
    provider::Sink sink;
    sink.context = &receive;
    sink.fixture = [](const provider::Fixture& row,
                      std::span<const provider::Event> events, void* raw) {
      auto& target = *static_cast<Receive*>(raw);
      return row.facts.id == target.omit || target.planner.consume(row, events);
    };
    const auto parsed = provider::readResponse(reader, next.spec, sink);
    assert(parsed.status == provider::Status::kOk);
    const bool complete = shared_planner.response(parsed);
    assert(complete == (omit == 0));
    return shared_planner.complete(
        complete ? plan::Fetch::kOk : plan::Fetch::kFailed, {utc, 100});
  };
  assert(shared_request(0).more);
  const auto& own_union = shared_planner.state(0).fixtures.own;
  std::set<uint32_t> planned_own;
  for (const auto id : own_union)
    if (id) assert(planned_own.insert(id).second);
  assert(planned_own ==
         std::set<uint32_t>(expected_own.begin(), expected_own.end()));
  assert(planned_own.size() > 8);
  assert(!shared_request(0).more);
  std::set<uint32_t> planned_matches;
  for (std::size_t i = 0; i < shared_store.matchCount(); ++i)
    assert(planned_matches.insert(shared_store.match(i).id).second);
  assert(planned_matches ==
         std::set<uint32_t>(expected_union.begin(), expected_union.end()));
  // A syntactically complete response missing a required own fixture beyond
  // bit7 must not finish the scope or prune previously valid data.
  assert(shared_request(0).more);
  const auto omitted = own_union[8];
  assert(omitted);
  assert(shared_request(omitted).outcome == schedule::Outcome::kFailed);
  assert(shared_store.find(1, cfg::Provider::kOpenLigaDb, omitted));
  // The enlarged temporary index still has one allocation. A real refusal at
  // that boundary reports RAM failure and cannot prune the previous scope.
  const auto retained_count = shared_store.matchCount();
  plan::Request no_memory_request;
  assert(shared_planner.request(shared_job, shared_routes.entries[0],
                                shared_store, {utc, 100}, no_memory_request));
  Input no_memory_input{shared_body};
  json::Reader no_memory_reader({&no_memory_input, Input::read});
  provider::Sink no_memory_sink;
  no_memory_sink.context = &shared_planner;
  no_memory_sink.fixture = [](const provider::Fixture& row,
                              std::span<const provider::Event> events,
                              void* raw) {
    return static_cast<plan::Planner*>(raw)->consume(row, events);
  };
  allocation_probe::deny_index = true;
  const auto failed_index = provider::readResponse(
      no_memory_reader, no_memory_request.spec, no_memory_sink);
  allocation_probe::deny_index = false;
  assert(allocation_probe::denied == 1 &&
         !shared_planner.response(failed_index) &&
         shared_planner.responseFailure() == provider::Status::kNoMemory);
  assert(shared_planner.complete(plan::Fetch::kFailed, {utc, 100}).outcome ==
         schedule::Outcome::kFailed);
  assert(shared_store.matchCount() == retained_count &&
         shared_store.find(1, cfg::Provider::kOpenLigaDb, omitted));
  // Resetting the planner reconstructs state in place; a fresh acquisition
  // must produce the same complete scope through the normal public functions.
  shared_planner.reset();
  assert(shared_request(0).more && !shared_request(0).more);
  assert(shared_store.matchCount() == retained_count);
  clubs.push_back(UINT32_MAX);
  retention::Index overflow(clubs, utc);
  retention::Selection rejected;
  assert(!overflow.select(rejected));
  // The real own-schedule endpoint is reverse ordered and has no round keys.
  const auto own_body = load(argv[3]);
  provider::RequestSpec own_request;
  own_request.source = {cfg::Provider::kEspn, "ger.1", "2025"};
  own_request.team = 132;
  own_request.operation = provider::Operation::kOwnFixtures;
  retention::Index own_index(132, parseUtc("2025-12-01T12:00:00Z"));
  Input own_input{own_body};
  json::Reader own_reader({&own_input, Input::read});
  provider::Sink own_sink;
  own_sink.context = &own_index;
  own_sink.fixture = [](const provider::Fixture& row,
                        std::span<const provider::Event>, void* raw) {
    return static_cast<retention::Index*>(raw)->observe(row);
  };
  assert(provider::readResponse(own_reader, own_request, own_sink).status ==
         provider::Status::kOk);
  retention::Selection own;
  assert(own_index.own(own));
  // The single club still has exactly four identities; the larger source
  // union has unused zero slots. Preserve the original four-fixture promise.
  assert(std::count_if(own.own.begin(), own.own.end(),
                       [](uint32_t id) { return id != 0; }) == 4);
  assert(!own_index.select(own));  // no invented competition round scope
  // The real cup calendar reports the two February weeks as ONE quarterfinal
  // stage. Keep the preceding Rd16 and following semifinals as complete stages.
  provider::RequestSpec cup_request;
  cup_request.source = {cfg::Provider::kEspn, "ger.dfb_pokal", "2025"};
  cup_request.operation = provider::Operation::kCalendarDays;
  provider::HttpRequest cup_http;
  assert(provider::buildRequest(cup_request, cup_http));
  assert(std::string_view(cup_http.url) ==
         "https://sports.core.api.espn.com/v2/sports/soccer/leagues/"
         "ger.dfb_pokal/seasons/2025/types/1/calendar/ondays");
  const auto cup_body = load(argv[4]);
  retention::Index calendar(0, parseUtc("2026-02-05T12:00:00Z"), {}, true);
  Input cup_input{cup_body};
  json::Reader cup_reader({&cup_input, Input::read});
  provider::Sink cup_sink;
  cup_sink.context = &calendar;
  cup_sink.day = [](uint32_t at, void* raw) {
    return static_cast<retention::Index*>(raw)->observeDay(at);
  };
  cup_sink.calendar_round = [](const provider::CalendarRound& round,
                               void* raw) {
    return static_cast<retention::Index*>(raw)->observeRound(round);
  };
  const auto cup_result =
      provider::readResponse(cup_reader, cup_request, cup_sink);
  assert(cup_result.status == provider::Status::kOk &&
         cup_result.metadata.metadata_complete &&
         std::string_view(cup_result.metadata.season.data) == "2025");
  MatchStore cup_store;
  retention::Selection stages;
  assert(calendar.select(stages, &cup_store, 1));
  assert(stages.count == 8 && stages.calendar_data.stage_count == 3);
  const char* expected_days[] = {"2025-12-02T08:00Z", "2025-12-03T08:00Z",
                                 "2026-02-03T08:00Z", "2026-02-04T08:00Z",
                                 "2026-02-10T08:00Z", "2026-02-11T08:00Z",
                                 "2026-04-22T07:00Z", "2026-04-23T07:00Z"};
  for (uint8_t i = 0; i < stages.count; ++i)
    assert(stages.calendar_data.days[i] == parseUtc(expected_days[i]));
  for (uint8_t i = 0; i < 3; ++i)
    assert(stages.calendar_data.stages[i].number == i + 3);
  assert(cup_store.roundCount() == 3 &&
         cup_store.round(1).state == DatasetState::kPartial);
  retention::Index ended(0, parseUtc("2026-07-02T12:00:00Z"), {}, true);
  Input ended_input{cup_body};
  json::Reader ended_reader({&ended_input, Input::read});
  cup_sink.context = &ended;
  assert(provider::readResponse(ended_reader, cup_request, cup_sink).status ==
         provider::Status::kOk);
  retention::Selection final;
  assert(ended.select(final));
  assert(final.count == 3 && final.calendar_data.stage_count == 2);
  assert(final.calendar_data.stages[0].number == 5 &&
         final.calendar_data.stages[1].number == 6);
  // A historical day really returns the explicit season's four fixtures.
  // Replaying those same valid facts for another requested day must not
  // establish that other day's complete scope.
  provider::RequestSpec day_request = cup_request;
  day_request.operation = provider::Operation::kFixtures;
  day_request.from = day_request.to =
      static_cast<uint32_t>(parseUtc("2025-12-02T00:00Z"));
  const auto day_body = load(argv[6]);
  Input day_input{day_body};
  json::Reader day_reader({&day_input, Input::read});
  const auto day_result = provider::readResponse(day_reader, day_request, {});
  assert(day_result.status == provider::Status::kOk && day_result.rows == 4 &&
         day_result.metadata.metadata_complete);
  day_request.from = day_request.to += 86400;
  Input wrong_day_input{day_body};
  json::Reader wrong_day_reader({&wrong_day_input, Input::read});
  const auto wrong_day =
      provider::readResponse(wrong_day_reader, day_request, {});
  assert(wrong_day.status == provider::Status::kBadResponse &&
         wrong_day.rows == 4 && wrong_day.metadata.invalid);
  cup_request.source.season = "2026";
  Input wrong_input{cup_body};
  json::Reader wrong_reader({&wrong_input, Input::read});
  assert(provider::readResponse(wrong_reader, cup_request, {}).status ==
         provider::Status::kBadResponse);
  // The production planner yields between the two requests. Failed admission
  // keeps earlier facts and cannot prune; only the complete replacement does.
  MatchStore runtime;
  plan::Planner planner;
  settings::CompetitionRoute route;
  route.id = 1;
  route.incarnation = 1;
  route.primary.provider = cfg::Provider::kOpenLigaDb;
  route.primary.competition.assign("bl1");
  route.primary.season.assign("2026");
  route.club.team = 40;
  StoredMatch old;
  old.id = 999999;
  old.competition = 1;
  old.provider = cfg::Provider::kOpenLigaDb;
  old.kickoff = static_cast<uint32_t>(utc - 30 * 86400);
  old.phase = Phase::kScheduled;
  runtime.upsert(old, {utc, 100});
  for (uint8_t competition = 2; competition <= 8; ++competition)
    for (uint32_t match = 0; match < 3 * 18; ++match) {
      auto row = old;
      row.id = 1000000 + competition * 1000 + match;
      row.competition = competition;
      runtime.upsert(row, {utc, 100});
    }
  const auto other_count = 7 * 3 * 18;
  const auto source_body = load(argv[1]);
  schedule::Job job;
  job.slot = 0;
  job.competition = 1;
  job.provider = cfg::Provider::kOpenLigaDb;
  job.type = cfg::DataType::kFixtures;
  const auto request_body = [&](bool truncated) {
    plan::Request next;
    assert(planner.request(job, route, runtime, {utc, 100}, next));
    auto bytes = source_body;
    if (truncated) bytes.resize(bytes.size() - 1);
    Input in{bytes};
    json::Reader json({&in, Input::read});
    provider::Sink sink;
    sink.context = &planner;
    sink.fixture = [](const provider::Fixture& row,
                      std::span<const provider::Event> events, void* raw) {
      return static_cast<plan::Planner*>(raw)->consume(row, events);
    };
    const auto parsed = provider::readResponse(json, next.spec, sink);
    const bool good =
        planner.response(parsed) && parsed.status == provider::Status::kOk;
    return planner.complete(good ? plan::Fetch::kOk : plan::Fetch::kFailed,
                            {utc, 100});
  };
  assert(request_body(false).more && runtime.matchCount() == other_count + 1);
  assert(request_body(true).outcome == schedule::Outcome::kFailed);
  assert(runtime.find(1, cfg::Provider::kOpenLigaDb, old.id));
  assert(request_body(false).more);
  assert(!request_body(false).more);
  assert(!runtime.find(1, cfg::Provider::kOpenLigaDb, old.id));
  assert(runtime.matchCount() ==
         other_count + static_cast<std::size_t>(argc - 8));
  assert(runtime.roundCount() == 3);
  schedule::Wanted wanted;
  planner.wanted(0, &route, runtime, {utc, 100}, wanted);
  assert(wanted.cost[static_cast<unsigned>(cfg::DataType::kFixtures)] == 2);
  std::cout
      << "PASS real306-game season: exact union before admission, arbitrary "
         "order, eight-source workload, two-request Planner/cost, failed "
         "document preserves scope; real34-game own schedule; actual cupQF "
         "two-weeks +previousRd16/nextsemi and strict explicit year; "
         "eight-club "
         "source union/dedup/order/capacity. Index="
      << sizeof(retention::Index)
      << " Selection=" << sizeof(retention::Selection) << '\n';
}
