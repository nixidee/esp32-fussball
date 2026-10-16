#include "provider_plan.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

#include "football_model.h"
#include "matchday.h"

namespace football::plan {
namespace {

using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonVariantConst;
using cfg::DataType;
using cfg::Provider;

constexpr uint32_t kDayS = 86400;
// Encoded components: three bytes per input byte plus the terminator.
constexpr std::size_t kCompetitionUrl = 3 * cfg::kCompetitionIdMaxBytes + 1;
constexpr std::size_t kSeasonUrl = 3 * cfg::kSeasonMaxBytes + 1;
constexpr std::size_t kTeamUrl = 3 * cfg::kTeamIdMaxBytes + 1;

constexpr const char* kOpenLigaDb = "https://api.openligadb.de/";
constexpr const char* kEspnSite =
    "https://site.api.espn.com/apis/site/v2/sports/soccer/";
constexpr const char* kApiFootball = "https://v3.football.api-sports.io/";
constexpr const char* kFootballData =
    "https://api.football-data.org/v4/competitions/";

std::size_t index(DataType type) { return static_cast<std::size_t>(type); }

uint32_t hash(uint32_t h, std::string_view text) {
  for (const char c : text) {
    h ^= static_cast<unsigned char>(c);
    h *= 16777619u;
  }
  return h;
}
uint32_t sourceKey(uint8_t id, const settings::Source& s) {
  const char head[2] = {static_cast<char>(id),
                        static_cast<char>(s.provider)};
  uint32_t h = hash(2166136261u, {head, 2});
  h = hash(h, s.competition.view());
  h = hash(hash(h, "/"), s.season.view());
  h = hash(hash(h, "/"), s.team.view());
  return h == 0 ? 1 : h;
}
bool sameLeague(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}
void date(int64_t utc, const char* format, char (&out)[16]) {
  const time_t stamp = static_cast<time_t>(utc);
  std::tm d{};
  gmtime_r(&stamp, &d);
  strftime(out, sizeof(out), format, &d);
}
void format(Request* out, const char* text, ...) {
  if (out == nullptr) return;
  va_list args;
  va_start(args, text);
  vsnprintf(out->url, sizeof(out->url), text, args);
  va_end(args);
}
void set(Request* out, Sink sink, const char* array, bool optional = false) {
  if (out == nullptr) return;
  out->sink = sink;
  out->array = array;
  out->optional = optional;
}
// Providers whose URLs need the season: the current one when none is set.
// Events (ESPN's match summary) never do.
// The resolved season is due for a new resolution at 'utc': its time is
// unknown, kSeasonRecheckS old, or after 'utc' (the clock was set back).
bool seasonStale(const SourceState& state, int64_t utc) {
  if (utc <= 0) return false;  // no valid clock: keep it
  return state.season_utc == 0 || utc < state.season_utc ||
         utc - state.season_utc >= cfg::kSeasonRecheckS;
}

bool needsSeason(Provider provider, DataType type) {
  if (type == DataType::kEvents) return false;
  if (provider == Provider::kApiFootball) return true;
  return provider == Provider::kOpenLigaDb && type != DataType::kFixtures;
}
// Expected requests per run of each data type at 'utc' ('known': the state
// belongs to this source).
void costs(const settings::Source& s, const SourceState& state, bool known,
           int64_t utc, std::array<uint8_t, cfg::kDataTypeCount>& cost) {
  const bool season = !s.season.empty() || (known && !state.season.empty());
  for (std::size_t t = 0; t < cfg::kDataTypeCount; ++t)
    cost[t] = needsSeason(s.provider, static_cast<DataType>(t)) && !season
                  ? 2
                  : 1;
  // The fixtures run resolves a due season again first (OQ-259, begin()).
  if (s.provider == Provider::kApiFootball && s.season.empty() && known &&
      seasonStale(state, utc))
    cost[index(DataType::kFixtures)] = 2;
  if (s.provider == Provider::kOpenLigaDb) {
    cost[index(DataType::kFixtures)] = s.team.empty() ? 2 : 3;
    cost[index(DataType::kLive)] = 2;  // change date, round when changed
  } else if (s.provider == Provider::kEspn) {
    // Match days, then one scoreboard per day (a week before the first).
    cost[index(DataType::kFixtures)] =
        static_cast<uint8_t>(1 + (known && state.day_count ? state.day_count
                                                           : 7));
  }
}
uint8_t bit(DataType type) { return static_cast<uint8_t>(1u << index(type)); }

// Data types the enabled screens of 'entry' need (OQ-192). The fixtures
// also give the live windows the live table follows.
uint8_t screenTypes(const settings::Competition& entry) {
  using Screen = cfg::CompetitionScreen;
  const bool club = !entry.primary.team.empty();
  const bool matches = (club && entry.uses(Screen::kOwnMatch)) ||
                       entry.uses(Screen::kConference) ||
                       entry.uses(Screen::kMatchday);
  const bool table = entry.uses(Screen::kTable);
  uint8_t types = 0;
  if (matches || table) types |= bit(DataType::kFixtures);
  if (matches) types |= bit(DataType::kLive);
  if (table) types |= bit(DataType::kTable) | bit(DataType::kLiveTable);
  return types;
}

// The match an own-match events run asks for: the latest own match of the
// source whose live window has opened at 'utc', once it has kicked off
// (also after its window ended: the final request); null otherwise.
const StoredMatch* eventsMatch(const MatchStore& store, uint8_t competition,
                               Provider provider, int64_t utc) {
  const StoredMatch* latest = nullptr;
  uint32_t latest_start = 0;
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    uint32_t start = 0, end = 0;
    if (m.competition != competition || m.provider != provider ||
        !m.isOwn() || !schedule::liveWindow(m, start, end) || start > utc ||
        (latest != nullptr && start < latest_start))
      continue;
    latest = &m;
    latest_start = start;
  }
  if (latest == nullptr) return nullptr;
  const Phase phase = latest->phase;
  return running(phase) || phase == Phase::kFinished ||
                 phase == Phase::kAbandoned
             ? latest
             : nullptr;
}

SourceState::Mark* findMark(SourceState& state, uint32_t round) {
  for (auto& mark : state.marks)
    if (mark.round != 0 && mark.round == round) return &mark;
  return nullptr;
}

}  // namespace

void encode(std::string_view in, char* out, std::size_t size) {
  if (size == 0) return;
  std::size_t at = 0;
  for (const char ch : in) {
    const auto c = static_cast<unsigned char>(ch);
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                       c == '_' || c == '~';
    if (at + (plain ? 1 : 3) >= size) break;
    if (plain)
      out[at++] = static_cast<char>(c);
    else
      at += static_cast<std::size_t>(snprintf(out + at, 4, "%%%02X", c));
  }
  out[at] = '\0';
}

std::size_t matchdayDays(const uint32_t* days, std::size_t count,
                         int64_t utc, uint32_t* out, std::size_t capacity) {
  // Last day of the matchday starting at 'first'.
  auto last = [&](std::size_t first) {
    while (first + 1 < count && !matchday::starts(days[first], days[first + 1]))
      ++first;
    return first;
  };
  std::size_t i = 0;
  while (i < count) {
    const std::size_t end = last(i);
    if (static_cast<int64_t>(days[end]) + kDayS > utc) break;
    i = end + 1;
  }
  std::size_t written = 0;
  for (int matchday = 0; matchday < 2 && i < count; ++matchday) {
    const std::size_t end = last(i);
    for (; i <= end; ++i)
      if (written < capacity) out[written++] = days[i];
  }
  return written;
}

uint32_t changeHash(std::string_view text) {
  const uint32_t h = hash(2166136261u, text);
  return h == 0 ? 1 : h;
}

void Planner::reset() {
  states_ = {};
  current_ = nullptr;
  store_ = nullptr;
}

void Planner::forget(uint8_t slot) {
  if (slot >= cfg::kCompetitionMax) return;
  if (current_ == &states_[slot]) abort();
  states_[slot] = {};
}

Resolved Planner::saved(uint8_t slot,
                        const settings::Competition& entry) const {
  Resolved out;
  if (slot >= cfg::kCompetitionMax) return out;
  const auto& state = states_[slot];
  if (state.key != sourceKey(entry.id, entry.primary)) return out;
  out.season = state.season;
  out.season_utc = state.season_utc;
  out.round = state.round;
  out.days = state.days;
  out.day_count = state.day_count;
  return out;
}

void Planner::restore(uint8_t slot, const settings::Competition& entry,
                      const Resolved& resolved) {
  if (slot >= cfg::kCompetitionMax || resolved.day_count > kEspnDays) return;
  auto& state = states_[slot];
  state = {};
  state.key = sourceKey(entry.id, entry.primary);
  state.season = resolved.season;
  state.season_utc = resolved.season_utc;
  state.round = resolved.round;
  state.days = resolved.days;
  state.day_count = resolved.day_count;
}

void Planner::wanted(uint8_t slot, const settings::Competition* entry,
                     const MatchStore& store, const Clock& clock,
                     schedule::Wanted& out) const {
  out = {};
  if (entry == nullptr || !entry->active || entry->id == 0 ||
      entry->primary.competition.empty() || slot >= cfg::kCompetitionMax)
    return;
  const auto& s = entry->primary;
  const auto& state = states_[slot];
  out.id = entry->id;
  out.provider = s.provider;
  out.types = screenTypes(*entry);
  // ESPN substitutions come only with the match summary (OQ-207).
  if (s.provider == Provider::kEspn && !s.team.empty() &&
      entry->uses(cfg::CompetitionScreen::kOwnMatch)) {
    out.types |= bit(DataType::kEvents);
    // Before kickoff the summary has nothing to ask for (begin()).
    if (eventsMatch(store, entry->id, s.provider, clock.utc) == nullptr)
      out.held |= bit(DataType::kEvents);
  }
  costs(s, state, state.key == sourceKey(entry->id, s), clock.utc, out.cost);
}

bool Planner::request(const schedule::Job& job,
                      const settings::Competition& entry, MatchStore& store,
                      const Clock& clock, Request& out) {
  current_ = nullptr;
  if (job.slot >= cfg::kCompetitionMax) return false;
  const auto& s = entry.primary;
  if (s.competition.empty()) return false;
  auto& state = states_[job.slot];
  const uint32_t key = sourceKey(entry.id, s);
  if (state.key != key) {
    state = {};
    state.key = key;
  }
  current_ = &state;
  store_ = &store;
  source_ = s;
  type_ = job.type;
  map_ = {};
  map_.competition = entry.id;
  map_.provider = s.provider;
  map_.club = decimalId(s.team.view());
  map_.clock = clock;
  return begin(store, clock, out);
}

bool Planner::begin(MatchStore& store, const Clock& clock, Request& out) {
  auto& state = *current_;
  auto& progress = state.progress[index(type_)];
  if (progress.step == 0 && progress.at == 0) {
    // A new run.
    if (type_ == DataType::kFixtures &&
        source_.provider == Provider::kApiFootball &&
        seasonStale(state, clock.utc))
      state.season = {};  // resolved again once a day (OQ-259)
    if (type_ == DataType::kLive) liveItems(store, clock.utc);
    // A fixtures run stores every match: its notices are settled when it
    // completes (OQ-311).
    if (type_ == DataType::kFixtures)
      store.beginRun(map_.competition, kNoticeFixtures);
    if (type_ == DataType::kEvents) {
      const auto* m = eventsMatch(store, map_.competition, source_.provider,
                                  clock.utc);
      event_match_ = m != nullptr ? m->id : 0;
    }
  }
  out = {};
  if (!build(&out)) {
    progress = {};
    current_ = nullptr;
    return false;
  }
  sink_ = out.sink;
  found_round_ = 0;
  found_change_ = 0;
  found_season_ = {};
  scan_count_ = scan_blocks_ = 0;
  scan_done_ = false;
  if (sink_ == Sink::kTable) store.beginTable(map_.competition);
  if (sink_ == Sink::kEvents) {
    // The match's own slot takes the events; without one nothing to ask.
    const auto* m = eventsRow();
    if (m == nullptr || !store.beginEvents(*m, true, map_.clock.utc)) {
      state.progress[index(type_)] = {};
      current_ = nullptr;
      return false;
    }
  }
  return true;
}

const StoredMatch* Planner::eventsRow() const {
  for (std::size_t i = 0; i < store_->matchCount(); ++i) {
    const auto& m = store_->match(i);
    if (m.id == event_match_ && m.competition == map_.competition &&
        m.provider == source_.provider)
      return &m;
  }
  return nullptr;
}

bool Planner::build(Request* out) {
  auto& state = *current_;
  auto& progress = state.progress[index(type_)];
  const auto& s = source_;
  const Provider p = s.provider;
  discover_ = filter_ = false;
  season_number_ = 0;
  char league[kCompetitionUrl], season[kSeasonUrl], team[kTeamUrl];
  encode(s.competition.view(), league, sizeof(league));
  encode(s.team.view(), team, sizeof(team));
  const auto season_text = s.season.empty() ? state.season.view()
                                            : s.season.view();
  encode(season_text, season, sizeof(season));

  if (progress.step == 0) {
    if (needsSeason(p, type_) && season_text.empty()) {
      if (p == Provider::kOpenLigaDb) {
        // The current round's matches carry the season (about 18 KB).
        format(out, "%sgetmatchdata/%s", kOpenLigaDb, league);
        set(out, Sink::kMatches, "");
        discover_ = true;
      } else {
        format(out, "%sleagues?id=%s&current=true", kApiFootball, league);
        set(out, Sink::kSeason, "response");
      }
      return true;
    }
    progress = {1, 0};
  }

  if (p == Provider::kOpenLigaDb) {
    switch (type_) {
      case DataType::kTable:
      case DataType::kLiveTable:
        if (progress.step != 1) return false;
        format(out, "%sgetbltable/%s/%s", kOpenLigaDb, league, season);
        set(out, Sink::kTable, "");
        return true;
      case DataType::kFixtures:
        if (progress.step == 1) {
          // Current round; its rows give the current season and round.
          format(out, "%sgetmatchdata/%s", kOpenLigaDb, league);
          set(out, Sink::kMatches, "");
          discover_ = true;
          filter_ = !s.season.empty();
          season_number_ = static_cast<int>(decimalId(s.season.view()));
          return true;
        }
        if (progress.step == 2) {
          if (state.round > 0 && !season_text.empty()) {
            format(out, "%sgetmatchdata/%s/%s/%u", kOpenLigaDb, league, season,
                   static_cast<unsigned>(state.round) + 1);
            set(out, Sink::kMatches, "");
            return true;
          }
          progress = {3, 0};
        }
        if (progress.step == 3 && !s.team.empty()) {
          // The club's fixtures within the horizon, also catch-up fixtures
          // outside the two rounds; other competitions' rows are skipped.
          const unsigned weeks = (cfg::kFixtureHorizonDays + 6) / 7;
          format(out, "%sgetmatchesbyteamid/%s/%u/%u", kOpenLigaDb, team,
                 weeks, weeks);
          set(out, Sink::kMatches, "");
          filter_ = true;
          season_number_ = static_cast<int>(decimalId(season_text));
          return true;
        }
        progress = {4, 0};
        return false;
      case DataType::kLive: {
        if (progress.at >= state.live_count) return false;
        const uint32_t round =
            state.live[progress.at] != 0 ? state.live[progress.at] : state.round;
        if (round == 0) {
          // The current round is unknown: its matches give it.
          format(out, "%sgetmatchdata/%s", kOpenLigaDb, league);
          set(out, Sink::kMatches, "");
          discover_ = true;
          filter_ = !s.season.empty();
          season_number_ = static_cast<int>(decimalId(s.season.view()));
          return true;
        }
        format(out, progress.step == 1 ? "%sgetlastchangedate/%s/%s/%lu"
                                       : "%sgetmatchdata/%s/%s/%lu",
               kOpenLigaDb, league, season, static_cast<unsigned long>(round));
        if (progress.step == 1)
          set(out, Sink::kChange, nullptr);
        else
          set(out, Sink::kMatches, "");
        return true;
      }
      case DataType::kEvents:
        return false;
    }
    return false;
  }

  if (p == Provider::kEspn) {
    char day[16];
    const char* with = s.season.empty() ? "" : "season=";
    const char* season_query = s.season.empty() ? "" : season;
    switch (type_) {
      case DataType::kTable:
      case DataType::kLiveTable:
        if (progress.step != 1) return false;
        // Cups answer without "entries": an empty table, not a failure.
        format(out,
               "https://site.web.api.espn.com/apis/v2/sports/soccer/%s/"
               "standings%s%s%s",
               league, s.season.empty() ? "" : "?", with, season_query);
        set(out, Sink::kTable, "entries", true);
        return true;
      case DataType::kFixtures:
        if (progress.step == 1) {
          format(out,
                 "https://sports.core.api.espn.com/v2/sports/soccer/leagues/"
                 "%s/calendar/ondays",
                 league);
          set(out, Sink::kDays, "dates");
          return true;
        }
        if (progress.at >= state.day_count) return false;
        date(state.days[progress.at], "%Y%m%d", day);
        format(out, "%s%s/scoreboard?dates=%s%s%s", kEspnSite, league, day,
               s.season.empty() ? "" : "&", with, season_query);
        set(out, Sink::kMatches, "events");
        return true;
      case DataType::kLive:
        if (progress.at >= state.live_count) return false;
        if (state.live[progress.at] == 0) {
          // No stored match day: the provider's next day with a match.
          format(out, "%s%s/scoreboard", kEspnSite, league);
        } else {
          date(state.live[progress.at], "%Y%m%d", day);
          format(out, "%s%s/scoreboard?dates=%s", kEspnSite, league, day);
        }
        set(out, Sink::kMatches, "events");
        return true;
      case DataType::kEvents:
        if (progress.step != 1 || event_match_ == 0) return false;
        format(out, "%s%s/summary?event=%lu", kEspnSite, league,
               static_cast<unsigned long>(event_match_));
        set(out, Sink::kEvents, "keyEvents", true);
        if (out != nullptr) out->body = cfg::kProviderSummaryBodyBytes;
        return true;
    }
    return false;
  }

  // Generic test sources: one request per run.
  if (progress.step != 1) return false;
  char from[16], to[16];
  const int64_t utc = map_.clock.utc;
  const bool live = type_ == DataType::kLive;
  const int64_t span = live ? 0 : cfg::kFixtureHorizonDays * int64_t{kDayS};
  date(utc - span, "%Y-%m-%d", from);
  // football-data.org v4 excludes the dateTo day (v4 changelog, checked
  // 2026-10-16); API-Football's 'to' includes it.
  date(utc + span + (p == Provider::kFootballData ? kDayS : 0), "%Y-%m-%d", to);
  if (p == Provider::kApiFootball) {
    switch (type_) {
      case DataType::kTable:
      case DataType::kLiveTable:
        format(out, "%sstandings?league=%s&season=%s", kApiFootball, league,
               season);
        set(out, Sink::kTable, "response");
        return true;
      case DataType::kFixtures:
      case DataType::kLive:
        format(out, "%sfixtures?league=%s&season=%s&from=%s&to=%s",
               kApiFootball, league, season, from, to);
        set(out, Sink::kMatches, "response");
        return true;
      case DataType::kEvents:
        return false;
    }
    return false;
  }
  const char* first = s.season.empty() ? "" : "?season=";
  const char* then = s.season.empty() ? "" : "&season=";
  switch (type_) {
    case DataType::kTable:
    case DataType::kLiveTable:
      format(out, "%s%s/standings%s%s", kFootballData, league, first, season);
      set(out, Sink::kTable, "standings");
      return true;
    case DataType::kFixtures:
    case DataType::kLive:
      format(out, "%s%s/matches?dateFrom=%s&dateTo=%s%s%s", kFootballData,
             league, from, to, then, season);
      set(out, Sink::kMatches, "matches");
      return true;
    case DataType::kEvents:
      return false;
  }
  return false;
}

void Planner::liveItems(const MatchStore& store, int64_t utc) {
  auto& state = *current_;
  state.live_count = 0;
  auto add = [&](uint32_t item) {
    for (uint8_t i = 0; i < state.live_count; ++i)
      if (state.live[i] == item) return;
    if (state.live_count < kLiveItems) state.live[state.live_count++] = item;
  };
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    uint32_t start = 0, end = 0;
    if (m.competition != map_.competition ||
        !schedule::liveWindow(m, start, end) || utc < start || utc >= end)
      continue;
    if (source_.provider == Provider::kOpenLigaDb) {
      // Round numbers are the provider's own.
      if (m.provider == Provider::kOpenLigaDb && m.round != 0) add(m.round);
    } else if (source_.provider == Provider::kEspn) {
      // The provider day holding the kickoff, else the next match day.
      uint32_t day = 0;
      for (uint8_t d = 0; d < state.day_count; ++d)
        if (state.days[d] <= m.kickoff && m.kickoff < state.days[d] + kDayS)
          day = state.days[d];
      add(day);
    }
  }
  // No round of its own: the current round (0 = still unknown, resolved by
  // the request).
  if (source_.provider == Provider::kOpenLigaDb && state.live_count == 0)
    add(state.round);
  if (source_.provider == Provider::kEspn && state.live_count == 0) add(0);
}

bool Planner::consume(JsonVariantConst element, void* planner) {
  return static_cast<Planner*>(planner)->consumeElement(element);
}

void Planner::addDay(uint32_t day) {
  if (scan_done_ || (scan_count_ > 0 && day <= scan_[scan_count_ - 1]))
    return;
  if (scan_count_ > 0 && matchday::starts(scan_[scan_count_ - 1], day)) {
    // The matchday before 'day' is complete.
    if (static_cast<int64_t>(scan_[scan_count_ - 1]) + kDayS <=
        map_.clock.utc)
      scan_count_ = 0;  // it has ended
    else if (++scan_blocks_ == 2) {
      scan_done_ = true;
      return;
    }
  }
  if (scan_count_ < scan_.size()) scan_[scan_count_++] = day;
}

bool Planner::consumeElement(JsonVariantConst row) {
  switch (sink_) {
    case Sink::kMatches:
      if (map_.provider == Provider::kOpenLigaDb) {
        if (discover_) {
          const int round = row["group"]["groupOrderID"] | 0;
          if (round > 0 && round < 256 && round > found_round_)
            found_round_ = round;
          if (row["leagueSeason"].is<int>()) {
            char text[cfg::kSeasonMaxBytes + 1];
            snprintf(text, sizeof(text), "%d", row["leagueSeason"].as<int>());
            found_season_.assign(text);
          }
        }
        if (filter_) {
          const char* shortcut = row["leagueShortcut"] | "";
          if (!sameLeague(shortcut, source_.competition.view()) ||
              (season_number_ != 0 &&
               (row["leagueSeason"] | 0) != season_number_))
            return true;
        }
      }
      storeMatch(row, *store_, map_);  // a row without id is skipped
      return true;
    case Sink::kTable:
      storeTableRow(row, *store_, map_);
      return true;
    case Sink::kChange:
      if (!row.is<const char*>()) return false;
      found_change_ = changeHash(row.as<const char*>());
      return true;
    case Sink::kDays: {
      const int64_t day = parseUtc(row | "");
      if (day > 0 && day < INT64_C(0x100000000))
        addDay(static_cast<uint32_t>(day));
      return true;
    }
    case Sink::kEvents:
      if (const auto* m = eventsRow()) storeSummaryEvent(row, *store_, *m);
      return true;
    case Sink::kSeason:
      for (JsonVariantConst item : row["seasons"].as<JsonArrayConst>())
        if (item["current"].as<bool>() && item["year"].is<int>()) {
          char text[cfg::kSeasonMaxBytes + 1];
          snprintf(text, sizeof(text), "%d", item["year"].as<int>());
          found_season_.assign(text);
        }
      return true;
  }
  return true;
}

Step Planner::finish(bool ok, const Clock& clock) {
  if (current_ == nullptr) return Step::kDone;
  if (sink_ == Sink::kTable) store_->commitTable(ok);
  if (sink_ == Sink::kEvents) store_->endEvents(ok);
  return advance(ok, clock);
}

void Planner::abort() {
  if (current_ == nullptr) return;
  current_->progress[index(type_)] = {};
  release();
}

void Planner::release() {
  if (current_ == nullptr) return;
  if (sink_ == Sink::kTable) store_->commitTable(false);
  if (sink_ == Sink::kEvents) store_->endEvents(false);
  current_ = nullptr;
}

Completion Planner::complete(Fetch fetch, const Clock& clock) {
  using schedule::Outcome;
  Completion c;
  switch (fetch) {
    case Fetch::kOk:
      switch (finish(true, clock)) {
        case Step::kDone: break;
        case Step::kMore: c.more = true; break;
        case Step::kInvalid:
          c.outcome = Outcome::kFailed;
          c.invalid = true;
          break;
      }
      return c;
    case Fetch::kFailed:
    case Fetch::kRateLimited:
      finish(false, clock);
      c.outcome =
          fetch == Fetch::kFailed ? Outcome::kFailed : Outcome::kRateLimited;
      return c;
    case Fetch::kBusy:
      release();
      c.outcome = Outcome::kDeferred;
      return c;
    case Fetch::kInterrupted:
    case Fetch::kCapped:
      abort();
      c.outcome = Outcome::kDeferred;
      return c;
    case Fetch::kRefused:
      abort();
      c.outcome = Outcome::kFailed;
      return c;
  }
  return c;
}

Step Planner::advance(bool ok, const Clock& clock) {
  auto& state = *current_;
  auto& progress = state.progress[index(type_)];
  auto end = [&](Step step) {
    progress = {};
    current_ = nullptr;
    return step;
  };
  if (!ok) return end(Step::kDone);
  const Provider p = source_.provider;
  if (progress.step == 0) {
    // Season request (OpenLigaDB rows also give the current round).
    if (found_season_.empty()) return end(Step::kInvalid);
    state.season = found_season_;
    state.season_utc = clock.utc > 0 && clock.utc < INT64_C(0x100000000)
                           ? static_cast<uint32_t>(clock.utc)
                           : 0;
    if (found_round_ > 0) state.round = static_cast<uint8_t>(found_round_);
    progress = {1, 0};
  } else if (p == Provider::kOpenLigaDb && type_ == DataType::kFixtures) {
    if (progress.step == 1) {
      if (found_round_ > 0) state.round = static_cast<uint8_t>(found_round_);
      if (source_.season.empty() && !found_season_.empty())
        state.season = found_season_;
    }
    ++progress.step;
  } else if (p == Provider::kOpenLigaDb && type_ == DataType::kLive) {
    const uint32_t round =
        state.live[progress.at] != 0 ? state.live[progress.at] : state.round;
    if (discover_) {
      // The current round's matches (the round was unknown).
      if (found_round_ > 0) state.round = static_cast<uint8_t>(found_round_);
      ++progress.at;
    } else if (progress.step == 1) {
      state.checked = found_change_;
      const auto* mark = findMark(state, round);
      const bool full = mark == nullptr || mark->change != state.checked ||
                        clock.mono_s - mark->at >= cfg::kOpenLigaDbFullRefreshS;
      if (full)
        progress.step = 2;
      else
        ++progress.at;
    } else {
      auto* mark = findMark(state, round);
      if (mark == nullptr) {
        // A free mark, else the one with the oldest full request.
        mark = &state.marks[0];
        for (auto& m : state.marks)
          if (mark->round != 0 && (m.round == 0 || m.at < mark->at)) mark = &m;
      }
      *mark = {static_cast<uint8_t>(round), state.checked, clock.mono_s};
      progress = {1, static_cast<uint8_t>(progress.at + 1)};
    }
  } else if (p == Provider::kEspn && type_ == DataType::kFixtures) {
    if (progress.step == 1) {
      state.day_count = static_cast<uint8_t>(matchdayDays(
          scan_.data(), scan_count_, clock.utc, state.days.data(),
          state.days.size()));
      progress = {2, 0};
    } else {
      ++progress.at;
    }
  } else if (p == Provider::kEspn && type_ == DataType::kLive) {
    ++progress.at;
  } else {
    ++progress.step;
  }
  if (!build(nullptr)) {
    if (type_ == DataType::kFixtures)
      store_->settleRun(map_.competition, kNoticeFixtures);
    return end(Step::kDone);
  }
  current_ = nullptr;
  return Step::kMore;
}

}  // namespace football::plan
