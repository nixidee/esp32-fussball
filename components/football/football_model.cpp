#include "football_model.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
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
Capabilities capabilities(cfg::Provider p) {
  switch (p) {
    case cfg::Provider::kOpenLigaDb:
      return {true, false, false, false, false,
              true, true,  false, false, cfg::kProviderMinIntervalMs[0]};
    case cfg::Provider::kApiFootball:
      return {true, true, true, true,  true,
              true, true, true, false, cfg::kProviderMinIntervalMs[1]};
    case cfg::Provider::kEspn:
      return {true, true, true,  true,  true,
              true, true, false, false, cfg::kProviderMinIntervalMs[2]};
    case cfg::Provider::kFootballData:
      return {true, true, false, false, false,
              true, true, true,  false, cfg::kProviderMinIntervalMs[3]};
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
  } else {
    m.phase = Phase::kLive;
    m.minute = {
        static_cast<int16_t>(std::min<int64_t>(
            elapsed > 60 ? elapsed - 15 : std::min<int64_t>(elapsed, 45), 120)),
        Source::kEstimated};
  }
}
bool matchWindow(const Match& m, int64_t utc, uint16_t before, uint16_t after) {
  if (!m.kickoff.valid() || m.phase == Phase::kPostponed ||
      m.phase == Phase::kCancelled || m.phase == Phase::kAbandoned)
    return false;
  const int64_t start = m.kickoff.value - before * 60;
  const int64_t maximum =
      m.kickoff.value + (cfg::kUnknownMatchEndMinutes + after) * 60;
  const int64_t end = m.finished_at.valid()
                          ? std::min(maximum, m.finished_at.value + after * 60)
                          : maximum;
  return utc >= start && utc < end;
}
bool conferenceWindow(const Snapshot& data, int64_t utc, uint16_t before,
                      uint16_t after) {
  for (unsigned i = 0; i < data.match_count; ++i) {
    const auto& anchor = data.matches[i];
    if (!anchor.kickoff.valid()) continue;
    int64_t start = INT64_MAX, end = 0;
    for (unsigned j = 0; j < data.match_count; ++j) {
      const auto& m = data.matches[j];
      if (m.route != anchor.route || m.provider != anchor.provider ||
          !m.kickoff.valid() || m.phase == Phase::kPostponed ||
          m.phase == Phase::kCancelled || m.phase == Phase::kAbandoned)
        continue;
      bool same_round;
      if (anchor.round.valid() && m.round.valid())
        same_round = anchor.round.value == m.round.value;
      else if (!anchor.round_label.empty() && !m.round_label.empty())
        same_round = anchor.round_label == m.round_label;
      else
        same_round = anchor.kickoff.value / 86400 == m.kickoff.value / 86400;
      if (!same_round) continue;
      start = std::min(start, m.kickoff.value - before * 60);
      const auto maximum =
          m.kickoff.value + (cfg::kUnknownMatchEndMinutes + after) * 60;
      end = std::max(end,
                     m.finished_at.valid()
                         ? std::min(maximum, m.finished_at.value + after * 60)
                         : maximum);
    }
    if (utc >= start && utc < end) return true;
  }
  return false;
}
uint32_t fingerprint(std::string_view text) {
  uint32_t h = 2166136261;
  for (unsigned char c : text) h = (h ^ c) * 16777619;
  return h;
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
