// Exact source proof identity, shared lookup, and fail-closed lifetime
// boundaries.
#include <cassert>
#include <cstdio>

#include "schedule_coverage.h"
using namespace football;
namespace c = football::coverage;
constexpr uint32_t kNow = 1800000000, kDay = 86400;
SourceKey key(std::string_view competition, std::string_view season) {
  SourceKey result;
  assert(result.assign(cfg::Provider::kOpenLigaDb, competition, season));
  return result;
}
void proof(c::EvidenceArray& evidence, const SourceKey& source,
           uint32_t previous, uint32_t next, uint32_t from, uint32_t to) {
  auto* entry = c::find(evidence, source);
  assert(entry);
  c::Collector collector;
  collector.begin(source, from, to, kNow, c::Proof::kFullSeason);
  unsigned rows = 0;
  if (previous) {
    collector.observe(previous);
    ++rows;
  }
  if (next) {
    collector.observe(next);
    ++rows;
  }
  const auto& span = collector.finish(kNow, {rows, true, true, true, true});
  assert(span.completeness == c::Completeness::kComplete);
  assert(c::accept(*entry, source, span) == c::Reason::kNone);
}
int main() {
  const auto current = key("bl1", "2026"), adjacent = key("bl1", "2027");
  c::Requirements requirements;
  for (uint8_t i = 0; i < requirements.size(); ++i)
    requirements[i] = {&current, uint8_t(i + 1)};
  c::EvidenceArray evidence;
  assert(c::synchronize(evidence, requirements));
  unsigned count = 0;
  for (const auto& entry : evidence) count += entry.source.valid();
  assert(count == 2 && c::find(evidence, current) &&
         c::find(evidence, adjacent));
  assert(!c::synchronize(evidence, requirements));
  const auto split = kNow - kDay;
  proof(evidence, current, kNow - 30 * kDay, 0, kNow - 40 * kDay, split);
  proof(evidence, adjacent, 0, kNow + 30 * kDay, split + 1, kNow + 40 * kDay);
  assert(c::assess(evidence, requirements, kNow, 21).verdict ==
         c::Verdict::kBreak);
  ++c::find(evidence, adjacent)->span.from;
  assert(c::assess(evidence, requirements, kNow, 21).reason ==
         c::Reason::kUncovered);
  --c::find(evidence, adjacent)->span.from;
  assert(c::observeNormal(evidence, current, split));
  assert(c::find(evidence, current)->span.reason == c::Reason::kTimingChanged);
  assert(c::find(evidence, adjacent)->span.reason == c::Reason::kTimingChanged);
  proof(evidence, adjacent, 0, kNow + 30 * kDay, split + 1, kNow + 40 * kDay);
  assert(c::assess(evidence, requirements, kNow, 21).verdict ==
         c::Verdict::kUnknown);
  proof(evidence, current, kNow - 30 * kDay, 0, kNow - 40 * kDay, split);
  assert(c::assess(evidence, requirements, kNow, 21).verdict ==
         c::Verdict::kBreak);
  assert(c::assess(evidence, requirements, kNow + cfg::kCoverageMaxAgeS, 21)
             .verdict == c::Verdict::kUnknown);
  assert(c::assess(evidence, requirements, kNow - 1, 21).reason ==
         c::Reason::kClock);
  requirements[7].source = nullptr;
  assert(c::assess(evidence, requirements, kNow, 21).verdict ==
         c::Verdict::kUnknown);
  requirements[7].source = &adjacent;
  assert(c::synchronize(evidence, requirements));
  count = 0;
  for (const auto& entry : evidence) count += entry.source.valid();
  assert(count == 3);  // overlapping current/adjacent preferences keep2027 once
  auto wrong = current;
  wrong.season.assign("2030");
  assert(c::accept(*c::find(evidence, current), wrong,
                   c::find(evidence, current)->span) == c::Reason::kIdentity);
  c::invalidateFamily(evidence, current, c::Reason::kMissingMetadata);
  proof(evidence, adjacent, 0, kNow + 30 * kDay, split + 1, kNow + 40 * kDay);
  assert(c::assess(evidence, requirements, kNow, 21).verdict ==
         c::Verdict::kUnknown);
  std::array<SourceKey, cfg::kCompetitionMax> distinct;
  for (uint8_t i = 0; i < distinct.size(); ++i) {
    char league[8];
    std::snprintf(league, sizeof(league), "league%u", unsigned(i));
    distinct[i] = key(league, "2026");
    requirements[i] = {&distinct[i], uint8_t(i + 1)};
  }
  assert(c::synchronize(evidence, requirements));
  count = 0;
  for (const auto& entry : evidence) {
    assert(entry.source.valid());
    ++count;
  }
  assert(count == cfg::kCoverageSources);
  assert(!c::find(evidence, current));
  assert(c::assess(evidence, requirements, kNow, 21).verdict ==
         c::Verdict::kUnknown);
  std::puts(
      "exact coverage: eight-view sharing, overlapping adjacent seasons, UTC "
      "join, family invalidation, stale/clock/unresolved/capacity PASS");
}
