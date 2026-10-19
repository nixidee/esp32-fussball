// Actual planner/parser/scheduler source sharing against recorded provider
// data.
#include <cassert>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "provider_plan.h"

namespace index_allocation {
bool deny = false;
unsigned refused = 0;
}  // namespace index_allocation
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  if (index_allocation::deny && bytes == sizeof(football::retention::Index)) {
    ++index_allocation::refused;
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

using namespace football;
namespace {
struct Body {
  std::string_view bytes;
  std::size_t at = 0;
  static int read(void* raw) {
    auto& body = *static_cast<Body*>(raw);
    return body.at < body.bytes.size()
               ? static_cast<unsigned char>(body.bytes[body.at++])
               : -1;
  }
};
std::string load(const char* path) {
  std::ifstream file(path);
  assert(file);
  return {std::istreambuf_iterator<char>(file), {}};
}
provider::ParseResult receive(plan::Planner& planner,
                              const plan::Request& request,
                              std::string_view bytes) {
  Body body{bytes};
  json::Reader reader({&body, Body::read});
  provider::Sink sink;
  sink.context = &planner;
  sink.fixture = [](const provider::Fixture& row,
                    std::span<const provider::Event> events, void* context) {
    return static_cast<plan::Planner*>(context)->consume(row, events);
  };
  sink.day = [](uint32_t day, void* context) {
    return static_cast<plan::Planner*>(context)->consumeDay(day);
  };
  sink.calendar_round = [](const provider::CalendarRound& round,
                           void* context) {
    return static_cast<plan::Planner*>(context)->consumeCalendarRound(round);
  };
  return provider::readResponse(reader, request.spec, sink);
}
void apiSeasonResolution() {
  const std::string valid =
      R"({"parameters":{"id":"78","current":"true"},"paging":{"current":1,"total":1},"results":1,"errors":[],"response":[{"league":{"id":78},"seasons":[{"year":2025,"current":false},{"year":2026,"current":true}]}]})";
  const auto check = [&](std::string bytes, bool expected) {
    settings::CompetitionRoutes routes;
    routes.entries.count = 1;
    auto& entry = routes.entries[0];
    entry.id = 1;
    entry.incarnation = 1;
    entry.primary.provider = cfg::Provider::kApiFootball;
    entry.primary.competition.assign("78");
    settings::screen_policy::Eligibility eligibility;
    MatchStore store;
    plan::Planner planner;
    planner.context(routes, eligibility);
    const Clock clock{1800000000, 100};
    planner.synchronize(store, clock);
    schedule::Job job;
    job.key.competition = 1;
    job.key.incarnation = 1;
    job.competition = 1;
    job.slot = cfg::kStoreSources;
    job.type = cfg::DataType::kFixtures;
    job.provider = entry.primary.provider;
    plan::Request request;
    assert(planner.request(job, entry, store, clock, request));
    const auto parsed = receive(planner, request, bytes);
    assert(planner.response(parsed) == expected);
    if (expected) {
      assert(parsed.metadata.metadata_complete &&
             parsed.metadata.source_matches);
      assert(planner.finish(true, clock) == plan::Step::kDone);
      const auto* source = store.sourceKey(store.currentSource(1));
      assert(source && source->season.view() == "2026" &&
             source->provider == cfg::Provider::kApiFootball);
      assert(store.matchCount() == 0);
    } else {
      planner.finish(false, clock);
      assert(store.currentSource(1) == kNoSource);
    }
  };
  check(valid, true);
  const auto bad = [&](std::string_view from, std::string_view to) {
    auto bytes = valid;
    const auto at = bytes.find(from);
    assert(at != std::string::npos);
    bytes.replace(at, from.size(), to);
    check(bytes, false);
  };
  bad(R"("id":78)", R"("id":79)");
  bad(R"("id":"78")", R"("id":"79")");
  bad(R"("current":"true")", R"("current":"false")");
  bad(R"("year":2025,"current":false)", R"("year":2025,"current":true)");
  bad(R"("year":2026,"current":true)", R"("year":2026,"current":false)");
  bad(R"("results":1)", R"("results":2)");
  bad(R"("total":1)", R"("total":2)");
  bad(R"("errors":[])", R"("errors":{"token":"invalid"})");
  check(valid.substr(0, valid.size() - 1), false);
}
void openLigaResolutionProvenance() {
  const auto row = [](unsigned id, std::string_view league, unsigned season) {
    return std::string{"{\"matchID\":"} + std::to_string(id) +
           ",\"leagueShortcut\":\"" + std::string(league) +
           "\",\"leagueSeason\":" + std::to_string(season) + "}";
  };
  const auto check = [&](const std::string& bytes, bool expected) {
    settings::CompetitionRoutes routes;
    routes.entries.count = 1;
    auto& entry = routes.entries[0];
    entry.id = 1;
    entry.incarnation = 1;
    entry.primary.competition.assign("bl1");
    settings::screen_policy::Eligibility eligibility;
    MatchStore store;
    plan::Planner planner;
    planner.context(routes, eligibility);
    const Clock clock{1800000000, 100};
    planner.synchronize(store, clock);
    schedule::Job job;
    job.competition = 1;
    job.key.competition = 1;
    job.key.incarnation = 1;
    job.slot = cfg::kStoreSources;
    job.type = cfg::DataType::kFixtures;
    plan::Request request;
    assert(planner.request(job, entry, store, clock, request));
    const auto parsed = receive(planner, request, bytes);
    assert(planner.response(parsed) == expected);
    if (expected) {
      assert(planner.finish(true, clock) == plan::Step::kDone);
      const auto* key = store.sourceKey(store.currentSource(1));
      assert(key && key->competition.view() == "bl1" &&
             key->season.view() == "2026");
      assert(!store.matchCount());
    } else {
      planner.finish(false, clock);
      assert(store.currentSource(1) == kNoSource);
    }
  };
  check("[" + row(1, "bl1", 2026) + "]", true);
  check("[" + row(1, "dfb", 2026) + "]", false);
  check("[" + row(1, "bl1", 2026) + "," + row(2, "dfb", 2026) + "]", false);
  check("[" + row(1, "bl1", 2026) + "," + row(2, "bl1", 2027) + "]", false);
}
void espnSharedSource(const char* folder, const char* oracle_path,
                      bool recovery) {
  std::ifstream oracle(oracle_path);
  assert(oracle);
  const auto read_ids = [&]() {
    unsigned count = 0;
    assert(oracle >> count);
    std::set<uint32_t> result;
    while (count--) {
      uint32_t id = 0;
      assert(oracle >> id);
      result.insert(id);
    }
    return result;
  };
  const auto expected = read_ids(), own125 = read_ids(), own598 = read_ids(),
             empty_future_expected = read_ids();
  settings::CompetitionRoutes routes;
  for (uint8_t i = 0; i < cfg::kCompetitionMax; ++i) {
    settings::CompetitionRoute entry;
    entry.id = i + 1;
    entry.incarnation = i + 1;
    entry.primary.provider = cfg::Provider::kEspn;
    entry.primary.competition.assign("ger.1");
    if (i) entry.primary.season.assign("2026");
    entry.club.team = i % 2 ? 598 : 125;
    assert(routes.entries.push(entry));
  }
  settings::screen_policy::Policy policy;
  auto eligibility = settings::screen_policy::eligibility(policy, routes);
  plan::Planner planner;
  planner.context(routes, eligibility);
  MatchStore store;
  schedule::Scheduler scheduler;
  settings::ProviderPlans plans;
  schedule::Inputs inputs;
  inputs.plans = &plans;
  inputs.store = &store;
  inputs.daily_left.fill(schedule::kUnlimited);
  Clock clock{parseUtc("2026-10-18T12:00:00Z"), 100};
  std::map<std::string, unsigned> requested;
  unsigned calls = 0;
  enum class ReplayFault { kNone, kMissingOwn, kIndexMemory, kEmptyFuture };
  const auto acquire = [&](ReplayFault fault) {
    const uint32_t omitted =
        fault == ReplayFault::kMissingOwn ? *own125.begin() : 0;
    bool complete = false;
    for (unsigned pass = 0; pass < 64 && !complete; ++pass) {
      planner.synchronize(store, clock);
      const auto changes = store.takeFixtureChanges();
      planner.restartFixtures(changes);
      scheduler.restartFixtures(changes);
      planner.prepareDemand(store, clock, inputs);
      inputs.clock = clock;
      for (auto& scope : inputs.scopes)
        scope.types &= 1u << static_cast<unsigned>(cfg::DataType::kFixtures);
      const auto decision = scheduler.next(inputs);
      if (!decision.run) {
        assert(decision.wake_s > clock.mono_s);
        clock.mono_s = decision.wake_s;
        continue;
      }
      const auto& job = decision.job;
      plan::Request request;
      assert(planner.request(job, routes.entries[job.route], store, clock,
                             request));
      provider::HttpRequest http;
      assert(provider::buildRequest(request.spec, http));
      std::string file;
      if (request.spec.operation == provider::Operation::kCalendarDays)
        file = "espn-calendar-2026.json";
      else if (request.spec.operation == provider::Operation::kOwnFixtures) {
        file = "espn-own-" + std::to_string(request.spec.team) +
               (std::string_view{http.url}.find("fixture=true") !=
                        std::string_view::npos
                    ? "-future-"
                    : "-past-") +
               "2026.json";
      } else {
        assert(request.spec.from);
        const time_t stamp = request.spec.from;
        std::tm civil{};
        gmtime_r(&stamp, &civil);
        char day[9];
        std::strftime(day, sizeof(day), "%Y%m%d", &civil);
        file = std::string{"espn-day-"} + day + ".json";
      }
      ++requested[file];
      auto bytes = load((std::string{folder} + "/" + file).c_str());
      if (fault == ReplayFault::kEmptyFuture &&
          request.spec.operation == provider::Operation::kOwnFixtures &&
          request.spec.upcoming)
        bytes =
            "{\"events\":[],\"requestedSeason\":{\"year\":2026},\"team\":{"
            "\"id\":" +
            std::to_string(request.spec.team) + "}}";
      assert(bytes.size() <= http.body_limit);
      scheduler.sent(job.provider, clock.mono_s);
      ++calls;
      const bool memory_refusal = fault == ReplayFault::kIndexMemory &&
                                  request.sink == plan::Sink::kOwnIndex &&
                                  request.spec.upcoming;
      const auto refused_before = index_allocation::refused;
      index_allocation::deny = memory_refusal;
      provider::ParseResult parsed;
      const bool dropping = omitted &&
                            request.sink == plan::Sink::kOwnMatches &&
                            request.spec.team == 125 &&
                            std::string_view{http.url}.find("fixture=true") !=
                                std::string_view::npos;
      if (dropping) {
        Body body{bytes};
        json::Reader reader({&body, Body::read});
        struct Target {
          plan::Planner& planner;
          uint32_t omitted;
        } target{planner, omitted};
        provider::Sink sink;
        sink.context = &target;
        sink.fixture = [](const provider::Fixture& row,
                          std::span<const provider::Event> events, void* raw) {
          auto& to = *static_cast<Target*>(raw);
          return row.facts.id == to.omitted || to.planner.consume(row, events);
        };
        parsed = provider::readResponse(reader, request.spec, sink);
      } else
        parsed = receive(planner, request, bytes);
      const bool accepted = planner.response(parsed);
      index_allocation::deny = false;
      if (!accepted && !dropping && !memory_refusal)
        std::fprintf(stderr, "ESPN reject %s status=%u response=%u rows=%u\n",
                     file.c_str(), unsigned(parsed.status),
                     unsigned(planner.responseFailure()), parsed.rows);
      if (dropping || memory_refusal) {
        assert(!accepted && planner.responseFailure() ==
                                (memory_refusal ? provider::Status::kNoMemory
                                                : provider::Status::kRejected));
        if (memory_refusal)
          assert(index_allocation::refused == refused_before + 1);
        const auto failed = planner.complete(plan::Fetch::kFailed, clock);
        assert(failed.outcome == schedule::Outcome::kFailed);
        scheduler.finished(job, failed.outcome, clock.mono_s, failed.more);
        clock.mono_s += 2;
        return false;
      }
      assert(accepted);
      const auto done = planner.complete(plan::Fetch::kOk, clock);
      assert(done.outcome == schedule::Outcome::kOk);
      scheduler.finished(job, done.outcome, clock.mono_s, done.more);
      complete = bool(job.key.source) && !done.more;
      clock.mono_s += 2;
    }
    assert(complete);
    return true;
  };
  assert(acquire(ReplayFault::kNone));
  std::set<uint32_t> actual;
  for (std::size_t i = 0; i < store.matchCount(); ++i)
    assert(actual.insert(store.match(i).id).second);
  if (actual != expected) {
    std::fprintf(stderr, "ESPN expected%zu actual%zu requests%u missing:",
                 expected.size(), actual.size(), calls);
    for (const auto id : expected)
      if (!actual.contains(id)) std::fprintf(stderr, " %u", id);
    std::fputc('\n', stderr);
  }
  assert(actual == expected);
  assert(calls == 18 && requested["espn-calendar-2026.json"] == 1);
  for (unsigned i = 0; i < routes.entries.size(); ++i) {
    const auto& entry = routes.entries[i];
    assert(store.currentSource(entry.id) == store.currentSource(1));
    const auto& own = entry.club.team == 125 ? own125 : own598;
    for (const auto id : own) {
      const auto* match = store.find(store.currentSource(entry.id), id);
      assert(match && store.isOwn(entry.id, *match));
    }
  }
  for (const auto team : {125, 598})
    for (const auto mode : {"past", "future"})
      assert(requested["espn-own-" + std::to_string(team) + "-" + mode +
                       "-2026.json"] == 2);
  planner.prepareDemand(store, clock, inputs);
  assert(inputs.scopes[store.currentSource(1)].cost[0] == 18);
  if (recovery) {
    const auto omitted = *own125.begin();
    const auto count = store.matchCount();
    scheduler.refresh();
    assert(!acquire(ReplayFault::kMissingOwn));
    assert(store.matchCount() == count &&
           store.find(store.currentSource(1), omitted));
    assert(store.datasetState(store.currentSource(1),
                              MatchStore::Dataset::kFixtures) ==
           DatasetState::kPartial);
    scheduler.refresh();
    assert(acquire(ReplayFault::kNone));
    std::set<uint32_t> recovered;
    for (std::size_t i = 0; i < store.matchCount(); ++i)
      recovered.insert(store.match(i).id);
    assert(recovered == expected);
    scheduler.refresh();
    assert(!acquire(ReplayFault::kIndexMemory));
    assert(store.matchCount() == count &&
           store.find(store.currentSource(1), omitted));
    scheduler.refresh();
    assert(acquire(ReplayFault::kNone));
    scheduler.refresh();
    assert(acquire(ReplayFault::kEmptyFuture));
    std::set<uint32_t> empty;
    for (std::size_t i = 0; i < store.matchCount(); ++i)
      empty.insert(store.match(i).id);
    assert(empty == empty_future_expected);
    scheduler.refresh();
    assert(acquire(ReplayFault::kNone));
    std::set<uint32_t> restored;
    for (std::size_t i = 0; i < store.matchCount(); ++i)
      restored.insert(store.match(i).id);
    assert(restored == expected);
  }
  std::cout << (recovery ? "current public ESPN"
                         : "controlled overlapping ESPN")
            << "8views/2clubs: exact " << expected.size()
            << "-match union, previous+next3, one calendar/18initial requests, "
               "shared facts"
            << (recovery ? ", missing-own/OOM preserve scope; empty future and "
                           "retries recover"
                         : "")
            << " PASS\n";
}
void sourceLifetime() {
  settings::CompetitionRoutes routes;
  routes.entries.count = 1;
  auto& entry = routes.entries[0];
  entry.id = 1;
  entry.incarnation = 9;
  entry.primary.competition.assign("bl1");
  entry.primary.season.assign("2026");
  MatchStore store;
  plan::Planner planner;
  settings::screen_policy::Eligibility eligibility;
  Clock clock{1800000000, 100};
  planner.context(routes, eligibility);
  planner.synchronize(store, clock);
  const auto old_slot = store.currentSource(1);
  const auto old_id = store.sourceId(old_slot);
  StoredMatch match;
  match.id = 42;
  match.source = old_slot;
  match.phase = Phase::kLive;
  match.round = 3;
  match.kickoff = clock.utc - 60;
  assert(store.upsert(match, clock));
  entry.primary.season.assign("2027");
  assert(planner.synchronize(store, clock));
  const auto next_id = store.sourceId(store.currentSource(1));
  assert(next_id && next_id != old_id && store.includes(1, old_slot));
  schedule::Inputs inputs;
  planner.prepareDemand(store, clock, inputs);
  bool old_live = false;
  for (const auto& scope : inputs.scopes)
    if (scope.key.source == old_id)
      old_live |=
          scope.types & (1u << static_cast<unsigned>(cfg::DataType::kLive));
  assert(old_live);
  schedule::Job job;
  job.key.source = old_id;
  job.competition = 1;
  job.type = cfg::DataType::kLive;
  plan::Request request;
  assert(planner.request(job, entry, store, clock, request));
  assert(request.spec.source.season == "2026" && request.spec.round == 3);
  provider::HttpRequest http;
  assert(provider::buildRequest(request.spec, http));
  assert(std::string_view(http.url).find("/bl1/2026/3") !=
         std::string_view::npos);
  planner.complete(plan::Fetch::kFailed, clock);
  clock.utc += 10 * 86400;
  clock.mono_s += 10 * 86400;
  assert(planner.synchronize(store, clock));
  assert(store.sourceSlot(old_id) == kNoSource && !store.find({42, old_id}));
  assert(!planner.request(job, entry, store, clock, request));
  // Reusing both the physical slot and provider ID cannot revive the old key.
  entry.primary.season.assign("2028");
  assert(planner.synchronize(store, clock));
  const auto reused = store.currentSource(1);
  assert(reused == old_slot && store.sourceId(reused) != old_id);
  match.source = reused;
  match.kickoff = clock.utc;
  assert(store.upsert(match, clock) && !store.find({42, old_id}));
  assert(!planner.request(job, entry, store, clock, request));
}
void capacityRecovery() {
  settings::CompetitionRoutes routes;
  routes.entries.count = 1;
  auto& entry = routes.entries[0];
  entry.id = 1;
  entry.incarnation = 9;
  entry.primary.competition.assign("bl1");
  MatchStore store;
  Clock clock{1800000000, 100};
  assert(store.bind(1, 9, 0, clock));
  std::array<SourceId, cfg::kStoreSources> ids;
  for (unsigned i = 0; i < ids.size(); ++i) {
    entry.primary.season.assign(std::to_string(2026 + i));
    SourceKey key;
    assert(key.assign(entry.primary.provider, entry.primary.competition.view(),
                      entry.primary.season.view()));
    ids[i] = store.resolve(1, key, clock);
    assert(ids[i]);
    StoredMatch row;
    row.id = 42;
    row.source = store.sourceSlot(ids[i]);
    row.kickoff = clock.utc + (i + 1) * 3600;
    assert(store.upsert(row, clock));
  }
  plan::Planner planner;
  settings::screen_policy::Eligibility eligibility;
  planner.context(routes, eligibility);
  planner.synchronize(store, clock);
  entry.primary.season.assign("2042");
  planner.synchronize(store, clock);
  assert(store.viewDatasetStatus(1, MatchStore::Dataset::kFixtures).cause ==
         Cause::kStoreFull);
  assert(store.matchCount() == ids.size() &&
         store.sourceId(store.currentSource(1)) == ids.back());
  for (const auto id : ids) assert(store.find({42, id}));
  clock.utc += 10 * 86400;
  clock.mono_s += 10 * 86400;
  planner.synchronize(store, clock);  // releases expired old sources
  planner.synchronize(store, clock);  // retries admission after release
  const auto* current = store.sourceKey(store.currentSource(1));
  assert(current && current->season.view() == "2042");
  assert(store.viewDatasetStatus(1, MatchStore::Dataset::kFixtures).cause ==
         Cause::kNone);
  assert(store.viewSummary(1).cause == Cause::kNone);
}
void staleResolution() {
  settings::CompetitionRoutes routes;
  routes.entries.count = 1;
  auto& entry = routes.entries[0];
  entry.id = 1;
  entry.incarnation = 2;
  entry.primary.competition.assign("bl1");
  MatchStore store;
  plan::Planner planner;
  settings::screen_policy::Eligibility eligibility;
  planner.context(routes, eligibility);
  const Clock clock{1800000000, 100};
  planner.synchronize(store, clock);
  schedule::Job old;
  old.key.competition = 1;
  old.key.incarnation = 1;
  old.competition = 1;
  old.slot = cfg::kStoreSources;
  plan::Request request;
  assert(!planner.request(old, entry, store, clock, request));
  old.key.incarnation = 2;
  assert(planner.request(old, entry, store, clock, request));
  planner.complete(plan::Fetch::kInterrupted, clock);
}
void rescheduledSeed() {
  const uint32_t now = 1800000000, day = 86400;
  retention::OwnFixtures seed{
      {{9, now - day}, {1, now + day}, {2, now + 2 * day}, {3, now + 3 * day}}};
  retention::Index index(125, now);
  index.seedOwn(125, seed);
  provider::Fixture row;
  row.source_matches = true;
  row.home.id = 125;
  row.facts.id = 1;
  row.facts.kickoff = now + 10 * day;
  assert(index.observe(row));
  row.facts.id = 4;
  row.facts.kickoff = now + 4 * day;
  assert(index.observe(row));
  retention::OwnFixtures chosen;
  assert(index.ownCandidates(125, chosen));
  std::set<uint32_t> actual;
  for (const auto& item : chosen)
    if (item.id) actual.insert(item.id);
  assert((actual == std::set<uint32_t>{9, 2, 3, 4}));
}
void rejectWrongEspnSeason() {
  settings::CompetitionRoutes routes;
  routes.entries.count = 1;
  auto& entry = routes.entries[0];
  entry.id = 1;
  entry.incarnation = 1;
  entry.active = true;
  entry.primary.provider = cfg::Provider::kEspn;
  entry.primary.competition.assign("ger.1");
  entry.primary.season.assign("2026");
  MatchStore store;
  plan::Planner planner;
  settings::screen_policy::Eligibility eligibility;
  planner.context(routes, eligibility);
  const Clock clock{1800000000, 100};
  planner.synchronize(store, clock);
  const auto slot = store.currentSource(1);
  StoredMatch match;
  match.id = 100;
  match.source = slot;
  match.phase = Phase::kLive;
  match.kickoff = clock.utc - 60;
  assert(store.upsert(match, clock));
  schedule::Job job;
  job.key.source = store.sourceId(slot);
  job.competition = 1;
  job.provider = cfg::Provider::kEspn;
  job.type = cfg::DataType::kLive;
  plan::Request request;
  assert(planner.request(job, entry, store, clock, request));
  const auto parsed = receive(
      planner, request,
      R"({"leagues":[{"slug":"ger.1","season":{"year":2025}}],"events":[]})");
  assert(!planner.response(parsed));
  assert(planner.complete(plan::Fetch::kFailed, clock).outcome ==
         schedule::Outcome::kFailed);
  assert(store.matchCount() == 1 && store.find({100, job.key.source}));
}
}  // namespace

int main(int argc, char** argv) {
  sourceLifetime();
  staleResolution();
  capacityRecovery();
  rescheduledSeed();
  apiSeasonResolution();
  openLigaResolutionProvenance();
  rejectWrongEspnSeason();
  assert(argc == 6);
  espnSharedSource(argv[3], argv[4], true);
  espnSharedSource(argv[5], argv[4], false);
  const auto bytes = load(argv[1]);
  std::ifstream oracle(argv[2]);
  unsigned count = 0;
  assert(oracle >> count && count == cfg::kCompetitionMax);
  std::vector<uint32_t> clubs(count);
  for (auto& club : clubs) assert(oracle >> club);
  assert(oracle >> count);
  std::set<uint32_t> expected;
  while (count--) {
    uint32_t id = 0;
    assert(oracle >> id && expected.insert(id).second);
  }
  settings::CompetitionRoutes routes;
  routes.entries.count = clubs.size();
  for (uint8_t i = 0; i < routes.entries.size(); ++i) {
    auto& entry = routes.entries[i];
    entry.id = i + 1;
    entry.incarnation = i + 1;
    entry.active = true;
    entry.primary.provider = cfg::Provider::kOpenLigaDb;
    entry.primary.competition.assign("bl1");
    if (i) entry.primary.season.assign("2026");
    entry.club.team = clubs[i];
  }
  settings::screen_policy::Eligibility eligibility;
  plan::Planner planner;
  planner.context(routes, eligibility);
  MatchStore store;
  schedule::Scheduler scheduler;
  settings::ProviderPlans plans;
  for (auto& plan : plans) plan.interval_s.fill(1000);
  schedule::Inputs inputs;
  inputs.plans = &plans;
  inputs.store = &store;
  inputs.daily_left.fill(schedule::kUnlimited);
  Clock clock{parseUtc("2026-10-14T12:00:00Z"), 100};
  const auto prepare = [&]() {
    planner.synchronize(store, clock);
    const auto changes = store.takeFixtureChanges();
    planner.restartFixtures(changes);
    scheduler.restartFixtures(changes);
    planner.prepareDemand(store, clock, inputs);
    inputs.clock = clock;
    return scheduler.next(inputs);
  };
  unsigned requests = 0;
  const auto run = [&](const schedule::Job& job) {
    plan::Request request;
    assert(
        planner.request(job, routes.entries[job.route], store, clock, request));
    scheduler.sent(job.provider, clock.mono_s);
    ++requests;
    const auto parsed = receive(planner, request, bytes);
    assert(parsed.status == provider::Status::kOk && parsed.document_complete);
    assert(planner.response(parsed));
    const auto done = planner.complete(plan::Fetch::kOk, clock);
    assert(done.outcome == schedule::Outcome::kOk && !done.invalid);
    scheduler.finished(job, done.outcome, clock.mono_s, done.more);
    clock.mono_s += (cfg::kRequestGapMs + 999) / 1000 + 1;
    return done;
  };
  auto decision = prepare();
  assert(decision.run && !decision.job.key.source);
  assert(decision.job.type == cfg::DataType::kFixtures);
  assert(!run(decision.job).more);
  assert(store.matchCount() == 0);  // Season discovery cannot admit fixtures.
  const auto shared = store.sourceId(store.currentSource(1));
  assert(shared);
  for (const auto& entry : routes.entries)
    assert(store.sourceId(store.currentSource(entry.id)) == shared);

  decision = prepare();
  assert(decision.run && decision.job.key.source == shared);
  assert(run(decision.job).more);
  assert(store.matchCount() == 0);  // Full index first, no partial admission.
  decision = prepare();
  assert(decision.run && decision.job.key.source == shared);
  assert(!run(decision.job).more);
  assert(requests == 3);  // One resolution, one shared index, one shared load.
  assert(store.matchCount() == expected.size());
  std::set<uint32_t> actual;
  for (std::size_t i = 0; i < store.matchCount(); ++i)
    assert(actual.insert(store.match(i).id).second);
  assert(actual == expected);
  assert(store.datasetState(store.sourceSlot(shared),
                            MatchStore::Dataset::kFixtures) ==
         DatasetState::kComplete);
  planner.prepareDemand(store, clock, inputs);
  unsigned source_jobs = 0;
  for (const auto& scope : inputs.scopes)
    if (scope.key.source &&
        (scope.types & (1u << static_cast<unsigned>(cfg::DataType::kFixtures))))
      ++source_jobs;
  assert(source_jobs == 1);

  // A fresh club preference preserves known facts and requests a new complete
  // retention union through the same source job, never a second fact owner.
  const auto retained = store.matchCount();
  assert(store.bind(1, 1, 0, clock));
  decision = prepare();
  assert(decision.run && decision.job.key.source == shared &&
         decision.job.type == cfg::DataType::kFixtures);
  assert(store.matchCount() == retained);
  assert(run(decision.job).more);
  assert(!run(prepare().job).more);
  assert(store.sourceId(store.currentSource(1)) == shared);

  // Truncated index data cannot prune the last authoritative union.
  scheduler.refresh();
  decision = prepare();
  plan::Request request;
  assert(planner.request(decision.job, routes.entries[decision.job.route],
                         store, clock, request));
  const auto before = store.matchCount();
  const auto broken = receive(
      planner, request, std::string_view(bytes).substr(0, bytes.size() / 2));
  assert(broken.status != provider::Status::kOk && !broken.document_complete);
  assert(!planner.response(broken));
  planner.complete(plan::Fetch::kFailed, clock);
  assert(store.matchCount() == before);
  std::cout << "recorded " << bytes.size()
            << "B/306 fixtures: eight clubs, equal "
            << "auto/explicit season, one shared data job, exact union, three "
            << "initial requests, club renewal and truncated-index "
               "preservation PASS; "
            << "Planner=" << sizeof(planner)
            << " SourceState=" << sizeof(plan::SourceState)
            << " Scheduler=" << sizeof(scheduler)
            << " Inputs=" << sizeof(inputs) << '\n';
}
