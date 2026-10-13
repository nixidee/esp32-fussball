#include "football_model.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>
namespace football {
void Snapshot::reset() noexcept {
  static_assert(std::is_trivially_copyable_v<Snapshot>);
  std::memset(static_cast<void*>(this), 0, sizeof(*this));
  stale = true;
}
Snapshot::Snapshot() noexcept { reset(); }
const char* providerName(cfg::Provider p) {
  switch (p) {
    case cfg::Provider::kOpenLigaDb: return "openligadb";
    case cfg::Provider::kApiFootball: return "api-football";
    case cfg::Provider::kEspn: return "espn";
    case cfg::Provider::kFootballData: return "football-data";
  }
  return "unknown";
}
const char* causeKey(Cause cause) {
  switch (cause) {
    case Cause::kNone: return "";
    case Cause::kNotStarted: return "not_started";
    case Cause::kProviderOff: return "provider_off";
    case Cause::kBudget: return "budget_day";
    case Cause::kBusy: return "busy";
    case Cause::kNetwork: return "net_lost";
    case Cause::kTime: return "time_invalid";
    case Cause::kSettings: return "settings_changed";
    case Cause::kRateLimited: return "rate_limited";
    case Cause::kAuth: return "auth";
    case Cause::kNotFound: return "not_found";
    case Cause::kHttp: return "http_error";
    case Cause::kConnect: return "connect";
    case Cause::kBadResponse: return "bad_response";
    case Cause::kTooLarge: return "too_large";
    case Cause::kMemory: return "memory";
    case Cause::kOther: return "other";
  }
  return "other";
}
const char* phaseName(Phase phase) {
  switch (phase) {
    case Phase::kUnknown: return "unknown";
    case Phase::kScheduled: return "scheduled";
    case Phase::kLive: return "live";
    case Phase::kHalfTime: return "half_time";
    case Phase::kExtraTime: return "extra_time";
    case Phase::kPenalties: return "penalties";
    case Phase::kFinished: return "finished";
    case Phase::kPostponed: return "postponed";
    case Phase::kCancelled: return "cancelled";
    case Phase::kAbandoned: return "abandoned";
  }
  return "unknown";
}
Capabilities capabilities(cfg::Provider p) {
  // Default request spacing (the device uses the saved one per provider).
  const auto default_spacing = [](unsigned i) {
    return cfg::kRequestSpacingDefaultS[i] * 1000u;
  };
  switch (p) {
    case cfg::Provider::kOpenLigaDb:
      return {true, false, false, false, false,
              true, true,  false, false, default_spacing(0)};
    case cfg::Provider::kApiFootball:
      return {true, true, true, true,  true,
              true, true, true, false, default_spacing(1)};
    case cfg::Provider::kEspn:
      return {true, true, true,  true,  true,
              true, true, false, false, default_spacing(2)};
    case cfg::Provider::kFootballData:
      return {true, true, false, false, false,
              true, true, true,  false, default_spacing(3)};
  }
  return {};
}
int64_t parseUtc(const char* text) {
  if (text == nullptr) return 0;
  const auto length = strlen(text);
  if (length < 16 || length > 40 || text[4] != '-' || text[7] != '-' ||
      text[10] != 'T' || text[13] != ':')
    return 0;
  auto digits = [&](unsigned start, unsigned count) {
    int result = 0;
    if (start + count > length) return -1;
    for (unsigned i = start; i < start + count; ++i) {
      if (text[i] < '0' || text[i] > '9') return -1;
      result = result * 10 + text[i] - '0';
    }
    return result;
  };
  int y = digits(0, 4), m = digits(5, 2), d = digits(8, 2);
  const int h = digits(11, 2), n = digits(14, 2);
  int s = 0;
  unsigned at = 16;
  if (text[at] == ':') {
    s = digits(at + 1, 2);
    at += 3;
  }
  if (y < 2020 || y > 2100 || m < 1 || m > 12 || d < 1 || h < 0 || h > 23 ||
      n < 0 || n > 59 || s < 0 || s > 59)
    return 0;
  const unsigned month_days[] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31};
  const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  if (static_cast<unsigned>(d) > month_days[m - 1] + (m == 2 && leap)) return 0;
  if (text[at] == '.') {
    const auto first = ++at;
    while (at < length && text[at] >= '0' && text[at] <= '9') ++at;
    if (at == first) return 0;
  }
  int offset = 0;
  if (text[at] == 'Z')
    ++at;
  else if (text[at] == '+' || text[at] == '-') {
    const int sign = text[at] == '+' ? 1 : -1;
    const int oh = digits(at + 1, 2), on = digits(at + 4, 2);
    if (at + 6 != length || text[at + 3] != ':' || oh < 0 || oh > 23 ||
        on < 0 || on > 59)
      return 0;
    offset = sign * (oh * 3600 + on * 60);
    at += 6;
  }
  if (at != length) return 0;
  // Gregorian days-to-epoch, independent of process TZ and libc timegm.
  y -= m <= 2;
  const int era = y / 400;
  const unsigned yo = y - era * 400;
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yo * 365 + yo / 4 - yo / 100 + doy;
  int64_t utc = (era * 146097LL + doe - 719468) * 86400 + h * 3600 + n * 60 + s;
  return utc - offset;
}
Phase statusPhase(const char* s) {
  if (!strcmp(s, "NS") || !strcmp(s, "TBD") || !strcmp(s, "TIMED") ||
      !strcmp(s, "SCHEDULED"))
    return Phase::kScheduled;
  if (!strcmp(s, "1H") || !strcmp(s, "2H") || !strcmp(s, "IN_PLAY") ||
      !strcmp(s, "LIVE"))
    return Phase::kLive;
  if (!strcmp(s, "HT") || !strcmp(s, "PAUSED")) return Phase::kHalfTime;
  if (!strcmp(s, "ET") || !strcmp(s, "EXTRA_TIME")) return Phase::kExtraTime;
  if (!strcmp(s, "P") || !strcmp(s, "PENALTY_SHOOTOUT"))
    return Phase::kPenalties;
  if (!strcmp(s, "FT") || !strcmp(s, "AET") || !strcmp(s, "PEN") ||
      !strcmp(s, "FINISHED"))
    return Phase::kFinished;
  if (!strcmp(s, "PST") || !strcmp(s, "POSTPONED") || !strcmp(s, "SUSP") ||
      !strcmp(s, "SUSPENDED"))
    return Phase::kPostponed;
  if (!strcmp(s, "CANC") || !strcmp(s, "CANCELLED")) return Phase::kCancelled;
  if (!strcmp(s, "ABD") || !strcmp(s, "AWD") || !strcmp(s, "ABANDONED") ||
      !strcmp(s, "AWARDED"))
    return Phase::kAbandoned;
  return Phase::kUnknown;
}
void estimate(Match& m, int64_t now) {
  if (!m.kickoff.valid() || confirmed(m.phase_source)) return;
  if (now < m.kickoff.value) {
    m.phase_source = Source::kEstimated;
    m.phase = Phase::kScheduled;
    m.minute = {};
    return;
  }
  const auto elapsed = (now - m.kickoff.value) / 60;
  m.phase_source = Source::kEstimated;
  if (elapsed < 0) {
    m.phase = Phase::kScheduled;
    m.minute = {};
  } else if (elapsed > cfg::kUnknownMatchEndMinutes) {
    m.phase = Phase::kUnknown;
    m.minute = {};
  } else if (elapsed > 45 && elapsed <= 60) {
    // Assumed 15-minute break after the first half.
    m.phase = Phase::kHalfTime;
    m.minute = {};
  } else {
    m.phase = Phase::kLive;
    m.minute = {
        static_cast<int16_t>(
            std::min<int64_t>(elapsed > 60 ? elapsed - 15 : elapsed, 120)),
        Source::kEstimated};
  }
}
bool scheduled(const Match& m) {
  return m.kickoff.valid() && m.phase != Phase::kPostponed &&
         m.phase != Phase::kCancelled && m.phase != Phase::kAbandoned;
}
bool running(const Match& m) {
  return m.phase == Phase::kLive || m.phase == Phase::kHalfTime ||
         m.phase == Phase::kExtraTime || m.phase == Phase::kPenalties;
}
int64_t matchEnd(const Match& m, uint16_t after) {
  const int64_t latest =
      m.kickoff.value + (cfg::kUnknownMatchEndMinutes + after) * 60;
  if (m.finished_at.valid())
    return std::min(latest, m.finished_at.value + after * 60);
  // Only a running phase reported by the provider extends the expected end
  // (extra time, penalties, delays); an estimated phase does not.
  if (running(m) && confirmed(m.phase_source)) return latest;
  return m.kickoff.value + (cfg::kExpectedMatchMinutes + after) * 60;
}
bool sameRound(const Match& a, const Match& b) {
  if (a.route != b.route || a.provider != b.provider) return false;
  if (a.round.valid() && b.round.valid()) return a.round.value == b.round.value;
  if (!a.round_label.empty() && !b.round_label.empty())
    return a.round_label == b.round_label;
  return a.kickoff.valid() && b.kickoff.valid() &&
         a.kickoff.value / 86400 == b.kickoff.value / 86400;
}
bool matchWindow(const Match& m, int64_t utc, uint16_t before, uint16_t after) {
  if (!scheduled(m)) return false;
  return utc >= m.kickoff.value - before * 60 && utc < matchEnd(m, after);
}
bool conferenceWindow(const Snapshot& data, int64_t utc, uint16_t before,
                      uint16_t after) {
  for (unsigned i = 0; i < data.match_count; ++i) {
    const auto& anchor = data.matches[i];
    if (!anchor.kickoff.valid()) continue;
    int64_t start = INT64_MAX, end = 0;
    for (unsigned j = 0; j < data.match_count; ++j) {
      const auto& m = data.matches[j];
      if (!scheduled(m) || !sameRound(anchor, m)) continue;
      start = std::min(start, m.kickoff.value - before * 60);
      end = std::max(end, matchEnd(m, after));
    }
    if (utc >= start && utc < end) return true;
  }
  return false;
}
void mergeEvents(Snapshot& primary, Match& a, const Snapshot& secondary,
                 const Match& b, bool swapped) {
  const auto scored = [](EventType type) {
    return type == EventType::kGoal || type == EventType::kOwnGoal ||
           type == EventType::kPenaltyGoal;
  };
  bool primary_scored = false, primary_missed = false;
  for (unsigned i = 0; i < a.event_count; ++i) {
    const auto type = primary.events[a.events[i]].type;
    primary_scored |= scored(type);
    primary_missed |= type == EventType::kPenaltyMissed;
  }
  for (unsigned i = 0; i < b.event_count; ++i) {
    const auto& event = secondary.events[b.events[i]];
    if (primary_scored && scored(event.type)) continue;
    if (primary_missed && event.type == EventType::kPenaltyMissed) continue;
    if (a.event_count >= a.events.size() ||
        primary.event_count >= primary.events.size()) {
      a.events_truncated = true;
      break;
    }
    auto& added = primary.events[primary.event_count];
    added = event;
    if (swapped) std::swap(added.after.home, added.after.away);
    a.events[a.event_count++] = primary.event_count++;
  }
  a.events_truncated |= b.events_truncated;
}
uint32_t fingerprint(std::string_view text) {
  uint32_t h = 2166136261;
  for (unsigned char c : text) h = (h ^ c) * 16777619;
  return h;
}
bool keepFixture(const Match& m, int64_t now, FixtureFilter filter) {
  if (m.id.empty()) return false;
  const bool own = std::string_view(filter.team) == m.home.id.data ||
                   std::string_view(filter.team) == m.away.id.data;
  if (m.round.valid() && filter.round && m.round.value == filter.round)
    return true;
  if (!m.kickoff.valid()) return own;
  if (now == 0) return true;
  return std::llabs(now - m.kickoff.value) <=
         (own ? cfg::kFixtureHorizonDays * 86400LL
              : cfg::kConferenceHorizonHours * 3600LL);
}
namespace {
// Removes match 'index' and its events from 'data'; later events and
// matches move down, so every event index stays valid.
void dropMatch(Snapshot& data, unsigned index) {
  auto& victim = data.matches[index];
  while (victim.event_count) {
    unsigned last = 0;
    for (unsigned i = 1; i < victim.event_count; ++i)
      if (victim.events[i] > victim.events[last]) last = i;
    const unsigned gone = victim.events[last];
    victim.events[last] = victim.events[--victim.event_count];
    for (unsigned e = gone + 1; e < data.event_count; ++e)
      data.events[e - 1] = data.events[e];
    --data.event_count;
    for (unsigned m = 0; m < data.match_count; ++m)
      for (unsigned i = 0; i < data.matches[m].event_count; ++i)
        if (data.matches[m].events[i] > gone) --data.matches[m].events[i];
  }
  for (unsigned m = index + 1; m < data.match_count; ++m)
    data.matches[m - 1] = data.matches[m];
  --data.match_count;
}
uint64_t distance(const Match& m, int64_t utc) {
  if (!m.kickoff.valid()) return UINT64_MAX;
  const int64_t d = m.kickoff.value - utc;
  return d < 0 ? 0 - static_cast<uint64_t>(d) : static_cast<uint64_t>(d);
}
}  // namespace
Match* matchSlot(Snapshot& target, const Match& candidate, int64_t utc) {
  if (target.match_count >= target.matches.size()) {
    unsigned victim = target.match_count;
    uint64_t farthest = 0;
    for (unsigned i = 0; utc && i < target.match_count; ++i) {
      const auto& m = target.matches[i];
      const auto d = distance(m, utc);
      if (!m.own && (victim == target.match_count || d > farthest)) {
        victim = i;
        farthest = d;
      }
    }
    target.matches_truncated = true;
    if (victim == target.match_count ||
        (!candidate.own && distance(candidate, utc) >= farthest))
      return nullptr;
    dropMatch(target, victim);
  }
  return &target.matches[target.match_count++];
}
bool appendMatch(Snapshot& target, const Snapshot& from, const Match& source,
                 int64_t utc, bool own_events_only, bool skip_removed) {
  auto* slot = matchSlot(target, source, utc);
  if (slot == nullptr) return false;
  auto& match = *slot;
  match = source;
  match.event_count = 0;
  for (unsigned i = 0; i < source.event_count; ++i) {
    const auto& event = from.events[source.events[i]];
    if ((own_events_only && event.provider != source.provider) ||
        (skip_removed && event.removed))
      continue;
    if (target.event_count >= target.events.size()) {
      match.events_truncated = true;
      break;
    }
    target.events[target.event_count] = event;
    match.events[match.event_count++] = target.event_count++;
  }
  return true;
}
bool carryRoute(Snapshot& target, const Snapshot& from, uint8_t route,
                bool table, int64_t utc) {
  bool complete = true;
  for (unsigned i = 0; i < from.match_count; ++i) {
    if (from.matches[i].route != route) continue;
    if (!appendMatch(target, from, from.matches[i], utc, false, true)) {
      complete = false;
      continue;
    }
    estimate(target.matches[target.match_count - 1], utc);
  }
  if (table && target.table_count == 0 && from.table_count) {
    target.table = from.table;
    target.table_count = from.table_count;
  }
  if (route < target.route_fetched_ms.size())
    target.route_fetched_ms[route] = from.route_fetched_ms[route];
  return complete;
}
int64_t oldestFetch(const Snapshot& data) {
  int64_t oldest = 0;
  for (const auto at : data.route_fetched_ms)
    if (at > 0 && (oldest == 0 || at < oldest)) oldest = at;
  return oldest ? oldest : data.fetched_ms;
}
void demo(Snapshot& out) {
  out.reset();
  out.configured = true;
  out.stale = false;
  out.match_count = 1;
  out.table_count = 5;
  auto& m = out.matches[0];
  m.id.assign("demo");
  m.own = true;
  m.home.id.assign("1");
  m.away.id.assign("2");
  m.home.name.assign("FC Bayern München");
  m.home.short_name.assign("Bayern");
  m.away.name.assign("Rot-Weiss Essen");
  m.away.short_name.assign("Essen");
  m.principal = {{2, Source::kProvider}, {1, Source::kProvider}};
  m.phase = Phase::kLive;
  m.phase_source = Source::kProvider;
  m.minute = {67, Source::kProvider};
  for (int i = 0; i < 5; ++i) {
    auto& r = out.table[i];
    r.team = i == 2 ? m.home : m.away;
    static constexpr const char* clubs[] = {"Demo United", "Demo City",
                                            "FC Bayern München",
                                            "Demo Athletic", "Rot-Weiss Essen"};
    r.team.name.assign(clubs[i]);
    r.team.short_name.assign(clubs[i]);
    r.own = i == 2;
    r.position = {static_cast<int16_t>(i + 1), Source::kProvider};
    r.points = {static_cast<int16_t>(30 - i * 3), Source::kProvider};
  }
}
}  // namespace football
