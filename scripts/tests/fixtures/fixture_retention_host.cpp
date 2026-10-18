#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>

#include "fixture_retention.h"
#include "provider_plan.h"
#include "store_mappers.h"

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
  assert(argc >= 8);
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
    for (int arg = 7; arg < argc; ++arg)
      expected.insert(static_cast<uint32_t>(std::stoul(argv[arg])));
    assert(store.matchCount() == expected.size());
    for (std::size_t i = 0; i < store.matchCount(); ++i)
      assert(expected.erase(store.match(i).id) == 1);
    assert(expected.empty() && store.teamCount() == 18 &&
           !(store.notices(1) & kNoticeMatches));
  }
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
  for (const auto id : own.own) assert(id != 0);
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
         other_count + static_cast<std::size_t>(argc - 7));
  assert(runtime.roundCount() == 3);
  schedule::Wanted wanted;
  planner.wanted(0, &route, runtime, {utc, 100}, wanted);
  assert(wanted.cost[static_cast<unsigned>(cfg::DataType::kFixtures)] == 2);
  std::cout
      << "PASS real306-game season: exact union before admission, arbitrary "
         "order, eight-source workload, two-request Planner/cost, failed "
         "document preserves scope; real34-game own schedule; actual cupQF "
         "two-weeks +previousRd16/nextsemi and strict explicit year. Index="
      << sizeof(retention::Index)
      << " Selection=" << sizeof(retention::Selection) << '\n';
}
