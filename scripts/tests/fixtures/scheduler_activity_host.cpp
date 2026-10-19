// Exercise the actual private activity assessment against an independent
// interval-union oracle. Includes its owner, never a copied implementation.
#include <cassert>
#include <chrono>
#include <cstdio>

#include "../../../components/football/request_scheduler.cpp"
using namespace football;
int main() {
  std::printf("policy %u %u %u\n", cfg::kLiveLeadMinutes,
              cfg::kExpectedMatchMinutes, cfg::kUnknownMatchEndMinutes);
  unsigned cases;
  assert(std::scanf("%u", &cases) == 1);
  for (unsigned c = 0; c < cases; ++c) {
    unsigned count, selected, target;
    long long now, end;
    assert(std::scanf("%u %u %u %lld %lld", &count, &selected, &target, &now,
                      &end) == 5);
    MatchStore store;
    SourceId sources[2];
    for (unsigned i = 0; i < 2; ++i) {
      SourceKey key;
      assert(
          key.assign(cfg::Provider::kOpenLigaDb, i ? "cup" : "league", "2026"));
      assert(store.bind(i + 1, i + 1, 0, {}));
      sources[i] = store.resolve(i + 1, key, {});
      assert(sources[i]);
    }
    for (unsigned i = 0; i < count; ++i) {
      unsigned source, kickoff, observed, finish, phase, estimated;
      assert(std::scanf("%u %u %u %u %u %u", &source, &kickoff, &observed,
                        &finish, &phase, &estimated) == 6);
      StoredMatch m;
      m.id = i + 1;
      m.source = store.sourceSlot(sources[source]);
      m.kickoff = kickoff;
      m.first_live_observed_utc = observed;
      m.finished_at = finish;
      constexpr Phase phases[]{Phase::kScheduled, Phase::kLive,
                               Phase::kFinished, Phase::kSuspended,
                               Phase::kCancelled};
      m.phase = phases[phase];
      m.estimated = estimated ? kEstimatedPhase : 0;
      assert(store.upsert(m, {}));
    }
    assert(store.matchCount() == count);
    std::array<MatchIndex, cfg::kStoreMatches> scratch;
    const auto a =
        schedule::activity(store, scratch, sources[selected], target, now, end);
    std::printf("%u %u %u %u %u %u\n", a.active, a.start, a.next, a.ended,
                a.seconds, a.windows);
    // Assessment only: source/display order and canonical facts are untouched.
    for (std::size_t i = 0; i < store.matchCount(); ++i) {
      const auto& m = store.match(i);
      assert(m.id && m.source < 2);
    }
  }
}
