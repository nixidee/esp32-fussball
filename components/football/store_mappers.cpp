#include "store_mappers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace football {
using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonVariantConst;
namespace {

const char* text(JsonVariantConst v) {
  return v.is<const char*>() ? v.as<const char*>() : "";
}

// A provider id: a positive integer, as number or digit string; 0 = none.
uint32_t numericId(JsonVariantConst v) {
  if (v.is<uint32_t>()) return v.as<uint32_t>();
  const char* s = text(v);
  if (*s < '0' || *s > '9') return 0;
  char* end;
  const unsigned long long n = strtoull(s, &end, 10);
  return *end == 0 && n <= UINT32_MAX ? static_cast<uint32_t>(n) : 0;
}

bool number(JsonVariantConst v, long min, long max, long& out) {
  if (v.isNull() || v.is<bool>()) return false;
  long value;
  if (v.is<int32_t>()) {
    value = v.as<int32_t>();
  } else if (v.is<double>()) {
    const double n = v.as<double>();
    if (!std::isfinite(n) || n < min || n > max || std::floor(n) != n)
      return false;
    value = static_cast<long>(n);
  } else if (v.is<const char*>()) {
    const char* s = text(v);
    char* end;
    value = strtol(s, &end, 10);
    if (s == end || *end != 0) return false;
  } else {
    return false;
  }
  if (value < min || value > max) return false;
  out = value;
  return true;
}

uint8_t small(JsonVariantConst v, long max = kNoValue - 1) {
  long n;
  return number(v, 0, max, n) ? static_cast<uint8_t>(n) : kNoValue;
}

int16_t signedNumber(JsonVariantConst v) {
  long n;
  return number(v, -10000, 10000, n) ? static_cast<int16_t>(n) : kNoNumber;
}

Goals goals(JsonVariantConst v, const char* home = "home",
            const char* away = "away") {
  return {small(v[home], 99), small(v[away], 99)};
}

uint32_t instant(JsonVariantConst v) {
  const int64_t t = parseUtc(text(v));
  return t > 0 && t <= UINT32_MAX ? static_cast<uint32_t>(t) : 0;
}

// Minute at the start of a provider clock text such as "64'" or "90'+3'".
uint8_t clockMinute(const char* clock) {
  char* end;
  const long minute = strtol(clock, &end, 10);
  return end != clock && minute >= 0 && minute <= 240
             ? static_cast<uint8_t>(minute)
             : kNoValue;
}

// Added minutes of a provider clock text such as "90'+3'" (none without).
uint8_t clockAdded(const char* clock) {
  const char* plus = strchr(clock, '+');
  if (plus == nullptr) return kNoValue;
  char* end;
  const long added = strtol(plus + 1, &end, 10);
  return end != plus + 1 && added >= 0 && added <= 120
             ? static_cast<uint8_t>(added)
             : kNoValue;
}

// A team of a match or table row: its directory index, provider id and full
// name. The short name falls back to the full name.
struct TeamRef {
  uint32_t id = 0;
  uint8_t index = kNoIndex;
  const char* name = "";
};

TeamRef team(JsonVariantConst v, MatchStore& store, const MapContext& c,
             const char* id_key = "id", const char* name_key = "name",
             const char* short_key = "shortName") {
  TeamRef t;
  t.id = numericId(v[id_key]);
  t.name = text(v[name_key]);
  const char* short_name = text(v[short_key]);
  if (c.pick == nullptr)
    t.index = store.team(c.competition, c.provider, t.id,
                         *short_name ? short_name : t.name);
  return t;
}

// A round label's pool index (none in pick mode).
uint8_t label(MatchStore& store, const MapContext& c, const char* text) {
  return c.pick == nullptr ? store.label(text) : kNoIndex;
}

bool isOwn(const MapContext& c, const TeamRef& home, const TeamRef& away) {
  return c.club != 0 && (home.id == c.club || away.id == c.club);
}

int value(uint8_t v) { return v == kNoValue ? -1 : v; }

// Revision and, unless the provider gives one, identity, then adds it.
void add(MatchStore& store, StoredEvent& e) {
  const int type = static_cast<int>(e.type);
  const int actor = static_cast<int>(e.actor);
  const int shootout = (e.flags & kEventShootout) ? 1 : 0;
  char revision[160];
  snprintf(revision, sizeof(revision), "%s|%d|%d|%d|%d|%d|%s|%d|%d|%d",
           e.player.data, type, value(e.minute), value(e.added),
           value(e.after.home), value(e.after.away), e.player_in.data, actor,
           static_cast<int>(e.beneficiary), shootout);
  e.revision = fingerprint(revision);
  if (e.identity == 0) {
    char identity[112];
    snprintf(identity, sizeof(identity), "%d|%d|%d|%d|%s|%s|%d", type,
             value(e.minute), value(e.added), actor, e.player.data,
             e.player_in.data, shootout);
    e.identity = fingerprint(identity);
  }
  store.addEvent(e);
}

bool scoring(EventType type) {
  return type == EventType::kGoal || type == EventType::kPenaltyGoal ||
         type == EventType::kOwnGoal;
}

Side opposite(Side s) {
  return s == Side::kHome   ? Side::kAway
         : s == Side::kAway ? Side::kHome
                            : Side::kUnknown;
}

// Where a mapped match goes (see MapContext::pairing): stored, filled into
// its paired primary match, or dropped. With 'events' (the row carries
// them) opens the event list of the own match that takes them
// (beginEvents()); false when none is open.
bool put(StoredMatch& m, MatchStore& store, const MapContext& c,
         const char* home_name, const char* away_name, bool events) {
  if (c.pick != nullptr) {
    if (m.isOwn()) c.pick(m.id, m.kickoff, home_name, away_name, c.pick_context);
    return false;
  }
  StoredMatch* target = nullptr;
  bool fallback = false, swapped = false;
  if (c.pairing == nullptr || c.pairing->standalone) {
    // A source of its own, or the fallback route: every row stands alone.
    target = store.upsert(m, c.clock, home_name, away_name);
  } else if (const auto* pair = c.pairing->find(c.competition, m.id)) {
    target = store.fill(m, c.pairing->primary, pair->primary, pair->swapped,
                        c.clock);
    fallback = true;
    swapped = pair->swapped;
  }
  return events && target != nullptr && target->isOwn() &&
         store.beginEvents(*target, fallback, swapped);
}

void openLigaGoals(JsonVariantConst row, MatchStore& store) {
  // The beneficiary only when consecutive totals identify the increment.
  int home = 0, away = 0;
  for (JsonVariantConst goal : row["goals"].as<JsonArrayConst>()) {
    StoredEvent e;
    e.identity = numericId(goal["goalID"]);
    e.type = goal["isOwnGoal"].as<bool>()   ? EventType::kOwnGoal
             : goal["isPenalty"].as<bool>() ? EventType::kPenaltyGoal
                                            : EventType::kGoal;
    e.minute = small(goal["matchMinute"], 240);
    e.player.assign(text(goal["goalGetterName"]));
    e.after = goals(goal, "scoreTeam1", "scoreTeam2");
    if (e.after.valid()) {
      if (e.after.home == home + 1 && e.after.away == away)
        e.beneficiary = Side::kHome;
      else if (e.after.away == away + 1 && e.after.home == home)
        e.beneficiary = Side::kAway;
      e.actor = e.type == EventType::kOwnGoal ? opposite(e.beneficiary)
                                              : e.beneficiary;
      home = e.after.home;
      away = e.after.away;
    }
    add(store, e);
  }
  store.endEvents(true);
}

bool openLigaMatch(JsonVariantConst row, MatchStore& store,
                   const MapContext& c) {
  StoredMatch m;
  m.competition = c.competition;
  m.provider = c.provider;
  m.id = numericId(row["matchID"]);
  if (m.id == 0) return false;
  const auto home =
      team(row["team1"], store, c, "teamId", "teamName", "shortName");
  const auto away =
      team(row["team2"], store, c, "teamId", "teamName", "shortName");
  m.home = home.index;
  m.away = away.index;
  if (isOwn(c, home, away)) m.flags |= kMatchOwn;
  m.kickoff = instant(row["matchDateTimeUTC"]);
  long round;
  if (number(row["group"]["groupOrderID"], 1, 255, round))
    m.round = static_cast<uint8_t>(round);
  m.label = label(store, c, text(row["group"]["groupName"]));
  Goals penalty_total;
  for (JsonVariantConst result : row["matchResults"].as<JsonArrayConst>()) {
    const Goals g = goals(result, "pointsTeam1", "pointsTeam2");
    const char* kind = text(result["resultTypeKind"]);
    if (!strcmp(kind, "HalfTime"))
      m.halftime = g;
    else if (!strcmp(kind, "After90Minutes"))
      m.regulation = g;
    else if (!strcmp(kind, "AfterExtraTime"))
      m.extra_time = g;
    else if (!strcmp(kind, "AfterPenalties"))
      penalty_total = g;
  }
  m.principal = m.extra_time.valid() ? m.extra_time : m.regulation;
  // OpenLigaDB stores the combined result after penalties, unlike ESPN's
  // separate shootoutScore. Convert only with a known pre-shootout total.
  if (penalty_total.valid() && m.extra_time.valid() &&
      penalty_total.home >= m.extra_time.home &&
      penalty_total.away >= m.extra_time.away)
    m.shootout = {static_cast<uint8_t>(penalty_total.home - m.extra_time.home),
                  static_cast<uint8_t>(penalty_total.away - m.extra_time.away)};
  if (row["matchIsFinished"].is<bool>() && row["matchIsFinished"].as<bool>())
    m.phase = Phase::kFinished;
  else
    estimate(m, c.clock.utc);
  if (put(m, store, c, home.name, away.name, true)) openLigaGoals(row, store);
  return true;
}

void espnMatch(JsonVariantConst row, StoredMatch& m, MatchStore& store,
               const MapContext& c, TeamRef& home, TeamRef& away) {
  m.id = numericId(row["id"]);
  auto competition = row["competitions"][0];
  m.kickoff = instant(row["date"]);
  auto type = competition["status"]["type"];
  const char* state = text(type["state"]);
  const char* status = text(type["name"]);
  m.phase = !strcmp(state, "post") && type["completed"].as<bool>()
                ? Phase::kFinished
            : !strcmp(state, "in")  ? Phase::kLive
            : !strcmp(state, "pre") ? Phase::kScheduled
                                    : Phase::kUnknown;
  if (strstr(status, "POSTPONED") || strstr(status, "SUSPENDED"))
    m.phase = Phase::kPostponed;
  else if (strstr(status, "CANCELED") || strstr(status, "CANCELLED"))
    m.phase = Phase::kCancelled;
  else if (strstr(status, "ABANDONED"))
    m.phase = Phase::kAbandoned;
  else if (!strcmp(status, "STATUS_HALFTIME"))
    m.phase = Phase::kHalfTime;
  else if (!strcmp(state, "in") && strstr(status, "PEN"))
    m.phase = Phase::kPenalties;
  else if (!strcmp(state, "in") && strstr(status, "EXTRA"))
    m.phase = Phase::kExtraTime;
  for (JsonVariantConst side : competition["competitors"].as<JsonArrayConst>()) {
    const char* where = text(side["homeAway"]);
    const bool is_home = !strcmp(where, "home");
    if (!is_home && strcmp(where, "away")) continue;
    (is_home ? home : away) = team(side["team"], store, c, "id", "displayName",
                                   "shortDisplayName");
    if (m.phase != Phase::kScheduled)
      (is_home ? m.principal.home : m.principal.away) =
          small(side["score"].is<JsonObjectConst>() ? side["score"]["value"]
                                                    : side["score"],
                99);
    (is_home ? m.shootout.home : m.shootout.away) =
        small(side["shootoutScore"], 99);
  }
  if (running(m.phase))
    m.minute = clockMinute(text(competition["status"]["displayClock"]));
}

void apiFootballMatch(JsonVariantConst row, StoredMatch& m, MatchStore& store,
                      const MapContext& c, TeamRef& home, TeamRef& away) {
  auto fixture = row["fixture"];
  m.id = numericId(fixture["id"]);
  m.kickoff = instant(fixture["date"]);
  home = team(row["teams"]["home"], store, c);
  away = team(row["teams"]["away"], store, c);
  m.phase = statusPhase(text(fixture["status"]["short"]));
  m.minute = small(fixture["status"]["elapsed"], 240);
  m.principal = goals(row["goals"]);
  m.halftime = goals(row["score"]["halftime"]);
  m.regulation = goals(row["score"]["fulltime"]);
  m.extra_time = goals(row["score"]["extratime"]);
  m.shootout = goals(row["score"]["penalty"]);
  m.label = label(store, c, text(row["league"]["round"]));
}

void footballDataMatch(JsonVariantConst row, StoredMatch& m, MatchStore& store,
                       const MapContext& c, TeamRef& home, TeamRef& away) {
  m.id = numericId(row["id"]);
  m.kickoff = instant(row["utcDate"]);
  home = team(row["homeTeam"], store, c);
  away = team(row["awayTeam"], store, c);
  m.phase = statusPhase(text(row["status"]));
  long round;
  if (number(row["matchday"], 1, 255, round))
    m.round = static_cast<uint8_t>(round);
  m.label = label(store, c, text(row["stage"]));
  auto s = row["score"];
  m.halftime = goals(s["halfTime"]);
  m.regulation = goals(s["regularTime"]);
  m.shootout = goals(s["penalties"]);
  const Goals extra = goals(s["extraTime"]);
  if (m.regulation.valid() && extra.valid())
    m.extra_time = {static_cast<uint8_t>(m.regulation.home + extra.home),
                    static_cast<uint8_t>(m.regulation.away + extra.away)};
  if (!strcmp(text(s["duration"]), "REGULAR")) {
    m.principal = goals(s["fullTime"]);
    m.regulation = m.principal;
  } else {
    m.principal = m.extra_time.valid() ? m.extra_time : m.regulation;
  }
}

Side side(const MatchStore& store, const StoredMatch& match, uint32_t team) {
  if (team == 0) return Side::kUnknown;
  if (match.home != kNoIndex && store.teamAt(match.home).id == team)
    return Side::kHome;
  if (match.away != kNoIndex && store.teamAt(match.away).id == team)
    return Side::kAway;
  return Side::kUnknown;
}

bool apiFootballEvent(JsonVariantConst row, StoredEvent& e) {
  const char* type = text(row["type"]);
  const char* detail = text(row["detail"]);
  if (!strcmp(type, "Goal"))
    e.type = !strcmp(detail, "Own Goal")         ? EventType::kOwnGoal
             : !strcmp(detail, "Penalty")        ? EventType::kPenaltyGoal
             : !strcmp(detail, "Missed Penalty") ? EventType::kPenaltyMissed
                                                 : EventType::kGoal;
  else if (!strcmp(type, "Card"))
    e.type = !strcmp(detail, "Red Card")             ? EventType::kRed
             : !strcmp(detail, "Second Yellow card") ? EventType::kYellowRed
                                                     : EventType::kYellow;
  else if (!strcmp(type, "subst"))
    e.type = EventType::kSubstitution;
  else if (!strcmp(type, "Var"))
    e.type = EventType::kVar;
  else
    return false;
  e.minute = small(row["time"]["elapsed"], 240);
  e.added = small(row["time"]["extra"], 120);
  e.player.assign(text(row["player"]["name"]));
  if (e.type == EventType::kSubstitution)
    e.player_in.assign(text(row["assist"]["name"]));
  return true;
}

bool espnEvent(JsonVariantConst row, StoredEvent& e) {
  const char* type = text(row["type"]["text"]);
  if (row["shootout"].as<bool>()) e.flags |= kEventShootout;
  if (row["ownGoal"].as<bool>())
    e.type = EventType::kOwnGoal;
  else if (row["penaltyKick"].as<bool>())
    e.type = row["scoringPlay"].as<bool>() ? EventType::kPenaltyGoal
                                           : EventType::kPenaltyMissed;
  else if (row["scoringPlay"].as<bool>() || strstr(type, "Goal"))
    e.type = EventType::kGoal;
  else if (strstr(type, "Second Yellow"))
    e.type = EventType::kYellowRed;
  else if (strstr(type, "Yellow"))
    e.type = EventType::kYellow;
  else if (strstr(type, "Red"))
    e.type = EventType::kRed;
  else if (strstr(type, "Substitution"))
    e.type = EventType::kSubstitution;
  else if (strstr(type, "Injury"))
    e.type = EventType::kInjury;
  else if (strstr(type, "VAR"))
    e.type = EventType::kVar;
  else
    return false;
  const char* first = text(row["athletesInvolved"][0]["displayName"]);
  if (e.type == EventType::kSubstitution) {
    // Both names in array order, "A / B": ESPN does not say who comes on.
    // Text::assign() cuts at a whole UTF-8 character.
    const char* second = text(row["athletesInvolved"][1]["displayName"]);
    char both[sizeof(e.player.data) + 1];
    snprintf(both, sizeof(both), *first && *second ? "%s / %s" : "%s%s", first,
             second);
    e.player.assign(both);
  } else {
    e.player.assign(first);
  }
  e.minute = clockMinute(text(row["clock"]["displayValue"]));
  return true;
}

bool tableRow(JsonVariantConst row, MatchStore& store, const MapContext& c) {
  StoredRow r;
  TeamRef t;
  if (c.provider == cfg::Provider::kOpenLigaDb) {
    t = team(row, store, c, "teamInfoId", "teamName", "shortName");
    r.position = static_cast<uint8_t>(
        std::min<std::size_t>(store.stagedRows() + 1, kNoValue - 1));
    r.flags |= kRowPositionEstimated;
    r.played = small(row["matches"]);
    r.won = small(row["won"]);
    r.drawn = small(row["draw"]);
    r.lost = small(row["lost"]);
    r.goals_for = small(row["goals"]);
    r.goals_against = small(row["opponentGoals"]);
    r.difference = signedNumber(row["goalDiff"]);
    r.points = signedNumber(row["points"]);
  } else if (c.provider == cfg::Provider::kApiFootball) {
    t = team(row["team"], store, c);
    r.position = small(row["rank"]);
    r.points = signedNumber(row["points"]);
    r.difference = signedNumber(row["goalsDiff"]);
    auto a = row["all"];
    r.played = small(a["played"]);
    r.won = small(a["win"]);
    r.drawn = small(a["draw"]);
    r.lost = small(a["lose"]);
    r.goals_for = small(a["goals"]["for"]);
    r.goals_against = small(a["goals"]["against"]);
  } else if (c.provider == cfg::Provider::kFootballData) {
    t = team(row["team"], store, c);
    r.position = small(row["position"]);
    r.played = small(row["playedGames"]);
    r.won = small(row["won"]);
    r.drawn = small(row["draw"]);
    r.lost = small(row["lost"]);
    r.goals_for = small(row["goalsFor"]);
    r.goals_against = small(row["goalsAgainst"]);
    r.difference = signedNumber(row["goalDifference"]);
    r.points = signedNumber(row["points"]);
  } else {
    t = team(row["team"], store, c, "id", "displayName", "shortDisplayName");
    for (JsonVariantConst s : row["stats"].as<JsonArrayConst>()) {
      const char* n = text(s["name"]);
      auto v = s["value"];
      if (!strcmp(n, "rank"))
        r.position = small(v);
      else if (!strcmp(n, "gamesPlayed"))
        r.played = small(v);
      else if (!strcmp(n, "points"))
        r.points = signedNumber(v);
      else if (!strcmp(n, "wins"))
        r.won = small(v);
      else if (!strcmp(n, "ties"))
        r.drawn = small(v);
      else if (!strcmp(n, "losses"))
        r.lost = small(v);
      else if (!strcmp(n, "pointsFor"))
        r.goals_for = small(v);
      else if (!strcmp(n, "pointsAgainst"))
        r.goals_against = small(v);
      else if (!strcmp(n, "pointDifferential"))
        r.difference = signedNumber(v);
    }
  }
  if (t.id == 0) return false;
  r.team = t.index;
  if (c.club != 0 && t.id == c.club) r.flags |= kRowOwn;
  // A row that does not fit is noted by the store; commitTable() then
  // keeps the stored table.
  store.appendRow(r);
  return true;
}

}  // namespace

bool storeMatch(JsonVariantConst row, MatchStore& store, const MapContext& c) {
  if (c.provider == cfg::Provider::kOpenLigaDb)
    return openLigaMatch(row, store, c);
  StoredMatch m;
  m.competition = c.competition;
  m.provider = c.provider;
  TeamRef home, away;
  if (c.provider == cfg::Provider::kApiFootball)
    apiFootballMatch(row, m, store, c, home, away);
  else if (c.provider == cfg::Provider::kFootballData)
    footballDataMatch(row, m, store, c, home, away);
  else
    espnMatch(row, m, store, c, home, away);
  if (m.id == 0) return false;
  m.home = home.index;
  m.away = away.index;
  if (isOwn(c, home, away)) m.flags |= kMatchOwn;
  if (m.phase == Phase::kUnknown) estimate(m, c.clock.utc);
  if (put(m, store, c, home.name, away.name,
          c.provider == cfg::Provider::kEspn)) {
    for (JsonVariantConst detail :
         row["competitions"][0]["details"].as<JsonArrayConst>())
      storeEvent(detail, store, m, c.provider);
    store.endEvents(true);
  }
  return true;
}

void storeEvent(JsonVariantConst row, MatchStore& store,
                const StoredMatch& match, cfg::Provider provider) {
  StoredEvent e;
  const bool known = provider == cfg::Provider::kApiFootball
                         ? apiFootballEvent(row, e)
                     : provider == cfg::Provider::kEspn ? espnEvent(row, e)
                                                        : false;
  if (!known) return;
  e.actor = side(store, match, numericId(row["team"]["id"]));
  if (scoring(e.type))
    e.beneficiary =
        e.type == EventType::kOwnGoal ? opposite(e.actor) : e.actor;
  add(store, e);
}

void storeSummaryEvent(JsonVariantConst row, MatchStore& store,
                       const StoredMatch& match) {
  if (strcmp(text(row["type"]["type"]), "substitution")) return;
  StoredEvent e;
  e.type = EventType::kSubstitution;
  e.identity = numericId(row["id"]);
  auto people = row["participants"];
  e.player_in.assign(text(people[0]["athlete"]["displayName"]));
  e.player.assign(text(people[1]["athlete"]["displayName"]));
  const char* clock = text(row["clock"]["displayValue"]);
  e.minute = clockMinute(clock);
  e.added = clockAdded(clock);
  e.actor = side(store, match, numericId(row["team"]["id"]));
  add(store, e);
}

const Pairing::Pair* Pairing::find(uint8_t competition,
                                   uint32_t secondary) const {
  for (uint8_t i = 0; i < count && i < pairs.size(); ++i)
    if (secondary != 0 && pairs[i].secondary == secondary &&
        pairs[i].competition == competition)
      return &pairs[i];
  return nullptr;
}

bool storeTableRow(JsonVariantConst row, MatchStore& store,
                   const MapContext& c) {
  if (c.provider == cfg::Provider::kApiFootball &&
      row["league"].is<JsonObjectConst>()) {
    for (JsonVariantConst group : row["league"]["standings"].as<JsonArrayConst>())
      for (JsonVariantConst item : group.as<JsonArrayConst>())
        if (!tableRow(item, store, c)) return false;
    return true;
  }
  if (c.provider == cfg::Provider::kFootballData &&
      row["table"].is<JsonArrayConst>()) {
    if (strcmp(text(row["type"]), "TOTAL")) return true;
    for (JsonVariantConst item : row["table"].as<JsonArrayConst>())
      if (!tableRow(item, store, c)) return false;
    return true;
  }
  return tableRow(row, store, c);
}

bool storeTeam(JsonVariantConst row, MatchStore& store, const MapContext& c) {
  JsonVariantConst t = row;
  const char *id = "id", *name = "name", *short_key = "shortName";
  if (c.provider == cfg::Provider::kOpenLigaDb) {
    id = "teamId";
    name = "teamName";
  } else if (c.provider == cfg::Provider::kEspn) {
    t = row["team"];
    name = "displayName";
    short_key = "shortDisplayName";
  } else if (c.provider == cfg::Provider::kApiFootball) {
    t = row["team"];
  }
  const uint32_t team = numericId(t[id]);
  if (team == 0) return false;
  const char* short_name = text(t[short_key]);
  store.rename(c.provider, team, *short_name ? short_name : text(t[name]));
  return true;
}

}  // namespace football
