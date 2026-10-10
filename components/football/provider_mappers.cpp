#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "provider_client.h"
namespace football {
using ArduinoJson::JsonVariantConst;
namespace {
const char* text(JsonVariantConst v) {
  return v.is<const char*>() ? v.as<const char*>() : "";
}
void id(Id& out, JsonVariantConst v) {
  if (v.is<const char*>())
    out.assign(text(v));
  else if (v.is<uint32_t>()) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%lu",
             static_cast<unsigned long>(v.as<uint32_t>()));
    out.assign(buffer);
  }
}
Value<int16_t> number(JsonVariantConst v, int min = 0, int max = 10000) {
  if (v.isNull() || v.is<bool>()) return {};
  long value;
  if (v.is<int32_t>())
    value = v.as<int32_t>();
  else if (v.is<double>()) {
    const double n = v.as<double>();
    if (!std::isfinite(n) || n < min || n > max || std::floor(n) != n)
      return {};
    value = static_cast<long>(n);
  } else if (v.is<const char*>()) {
    const auto* s = text(v);
    char* end;
    value = strtol(s, &end, 10);
    if (s == end || *end != 0) return {};
  } else
    return {};
  return value < min || value > max
             ? Value<int16_t>{}
             : Value<int16_t>{static_cast<int16_t>(value), Source::kProvider};
}
Value<int64_t> instant(JsonVariantConst v) {
  const auto time = parseUtc(text(v));
  return time > 0 ? Value<int64_t>{time, Source::kProvider} : Value<int64_t>{};
}
Score score(JsonVariantConst v, const char* home = "home",
            const char* away = "away") {
  return {number(v[home], 0, 99), number(v[away], 0, 99)};
}
Team team(JsonVariantConst v, const char* id_key = "id",
          const char* name_key = "name", const char* short_key = "shortName") {
  Team t;
  id(t.id, v[id_key]);
  t.name.assign(text(v[name_key]));
  t.short_name.assign(text(v[short_key]));
  if (t.short_name.empty()) t.short_name.assign(t.name.data);
  return t;
}
Phase phase(const char* s) {
  if (!strcmp(s, "NS") || !strcmp(s, "TBD") || !strcmp(s, "TIMED") ||
      !strcmp(s, "SCHEDULED"))
    return Phase::kScheduled;
  if (!strcmp(s, "1H") || !strcmp(s, "2H") || !strcmp(s, "IN_PLAY") ||
      !strcmp(s, "LIVE"))
    return Phase::kLive;
  if (!strcmp(s, "HT") || !strcmp(s, "PAUSED")) return Phase::kHalfTime;
  if (!strcmp(s, "ET")) return Phase::kExtraTime;
  if (!strcmp(s, "P")) return Phase::kPenalties;
  if (!strcmp(s, "FT") || !strcmp(s, "AET") || !strcmp(s, "PEN") ||
      !strcmp(s, "FINISHED"))
    return Phase::kFinished;
  if (!strcmp(s, "PST") || !strcmp(s, "POSTPONED") || !strcmp(s, "SUSP"))
    return Phase::kPostponed;
  if (!strcmp(s, "CANC") || !strcmp(s, "CANCELLED")) return Phase::kCancelled;
  if (!strcmp(s, "ABD") || !strcmp(s, "AWD") || !strcmp(s, "ABANDONED"))
    return Phase::kAbandoned;
  return Phase::kUnknown;
}
bool keep(const Match& m, int64_t now, FixtureFilter filter) {
  if (m.id.empty()) return false;
  const bool own = std::string_view(filter.team) == m.home.id.data ||
                   std::string_view(filter.team) == m.away.id.data;
  if (m.round.valid() && filter.round && m.round.value == filter.round)
    return true;
  if (!m.kickoff.valid()) return own;
  if (now == 0) return true;
  return std::llabs(now - m.kickoff.value) <=
         (own            ? cfg::kFixtureHorizonDays * 86400LL
          : filter.round ? 0
                         : cfg::kConferenceHorizonHours * 3600LL);
}
bool duplicate(const Snapshot& out, const Match& m) {
  for (unsigned i = 0; i < out.match_count; ++i)
    if (out.matches[i].route == m.route && out.matches[i].id == m.id)
      return true;
  return false;
}
Match* addMatch(Snapshot& out, Match& m, int64_t now, FixtureFilter filter) {
  if (!keep(m, now, filter)) return nullptr;
  for (unsigned i = 0; i < out.match_count; ++i)
    if (out.matches[i].route == m.route && out.matches[i].id == m.id)
      return nullptr;
  if (out.match_count >= out.matches.size()) return nullptr;
  auto* result = &out.matches[out.match_count++];
  *result = m;
  return result;
}
void appendEvent(Snapshot& out, Match& m, MatchEvent& e) {
  e.provider = m.provider;
  char revision[352];
  snprintf(
      revision, sizeof(revision), "%s|%s|%d|%d|%d|%d|%d|%s|%s|%s|%s|%d",
      e.player.data, e.detail.data, static_cast<int>(e.type), e.minute.value,
      e.added.value, e.after.home.valid() ? e.after.home.value : -1,
      e.after.away.valid() ? e.after.away.value : -1, e.player_in.data,
      e.player_out.data, e.actor_team.data, e.beneficiary.data, e.shootout);
  e.revision = fingerprint(revision);
  if (e.identity == 0) {
    char identity[176];
    snprintf(identity, sizeof(identity), "%d|%d|%d|%s|%s|%s|%d",
             static_cast<int>(e.type), e.minute.valid() ? e.minute.value : -1,
             e.added.valid() ? e.added.value : -1, e.actor_team.data,
             e.player.data, e.player_in.data, e.shootout);
    e.identity = fingerprint(identity);
  }
  if (m.event_count == m.events.size()) {
    // Reuse the displaced entry; an orphan must not consume the global pool.
    const auto reuse = m.events[0];
    for (unsigned i = 1; i < m.event_count; ++i) m.events[i - 1] = m.events[i];
    m.events[m.event_count - 1] = reuse;
    out.events[reuse] = e;
    m.events_truncated = true;
    return;
  }
  if (out.event_count >= out.events.size()) {
    m.events_truncated = true;
    return;
  }
  m.events[m.event_count++] = out.event_count;
  out.events[out.event_count++] = e;
}
}  // namespace
bool mapOpenLigaMatch(JsonVariantConst row, Snapshot& out, uint8_t route,
                      int64_t now, FixtureFilter filter) {
  Match m;
  m.route = route;
  id(m.id, row["matchID"]);
  m.home = team(row["team1"], "teamId", "teamName", "shortName");
  m.away = team(row["team2"], "teamId", "teamName", "shortName");
  m.own = std::string_view(filter.team) == m.home.id.data ||
          std::string_view(filter.team) == m.away.id.data;
  if (m.id.empty()) return false;
  m.kickoff = instant(row["matchDateTimeUTC"]);
  // A naive provider-local change timestamp is not a UTC instant.
  const char* change = text(row["lastUpdateDateTime"]);
  if (strchr(change, 'Z') ||
      (strlen(change) > 10 &&
       (strchr(change + 10, '+') || strchr(change + 10, '-'))))
    m.provider_changed = instant(row["lastUpdateDateTime"]);
  auto round = number(row["group"]["groupOrderID"], 1, 1000);
  if (round.valid())
    m.round = {static_cast<uint16_t>(round.value), round.source};
  m.round_label.assign(text(row["group"]["groupName"]));
  Score penalty_total;
  for (JsonVariantConst result :
       row["matchResults"].as<ArduinoJson::JsonArrayConst>()) {
    Score s = score(result, "pointsTeam1", "pointsTeam2");
    const char* kind = text(result["resultTypeKind"]);
    if (!strcmp(kind, "HalfTime"))
      m.halftime = s;
    else if (!strcmp(kind, "After90Minutes"))
      m.regulation = s;
    else if (!strcmp(kind, "AfterExtraTime"))
      m.extra_time = s;
    else if (!strcmp(kind, "AfterPenalties"))
      penalty_total = s;
  }
  m.principal = m.extra_time.valid() ? m.extra_time : m.regulation;
  // OpenLigaDB stores the combined result after penalties, unlike ESPN's
  // separate shootoutScore. Convert only with a known pre-shootout total.
  if (penalty_total.valid() && m.extra_time.valid() &&
      penalty_total.home.value >= m.extra_time.home.value &&
      penalty_total.away.value >= m.extra_time.away.value) {
    m.shootout = {{static_cast<int16_t>(penalty_total.home.value -
                                        m.extra_time.home.value),
                   Source::kProvider},
                  {static_cast<int16_t>(penalty_total.away.value -
                                        m.extra_time.away.value),
                   Source::kProvider}};
  }
  if (row["matchIsFinished"].is<bool>() && row["matchIsFinished"].as<bool>()) {
    m.phase = Phase::kFinished;
    m.phase_source = Source::kProvider;
  } else if (now > 0)
    estimate(m, now);
  const bool relevant = keep(m, now, filter);
  if (duplicate(out, m)) return true;
  if (relevant && out.match_count >= out.matches.size()) return false;
  auto* match = addMatch(out, m, now, filter);
  if (match == nullptr) return true;
  // Conference effects use score changes. Reserve rich event storage for
  // the club this product tracks, across its configured competitions.
  if (!match->own) return true;
  for (JsonVariantConst goal : row["goals"].as<ArduinoJson::JsonArrayConst>()) {
    MatchEvent e;
    if (goal["goalID"].is<uint32_t>())
      e.identity = goal["goalID"].as<uint32_t>();
    e.type = goal["isOwnGoal"].as<bool>()   ? EventType::kOwnGoal
             : goal["isPenalty"].as<bool>() ? EventType::kPenaltyGoal
                                            : EventType::kGoal;
    e.minute = number(goal["matchMinute"], 0, 240);
    e.player.assign(text(goal["goalGetterName"]));
    e.after = score(goal, "scoreTeam1", "scoreTeam2");
    appendEvent(out, *match, e);
  }
  // Derive beneficiary only when consecutive totals identify the increment.
  int home = 0, away = 0;
  for (unsigned i = 0; i < match->event_count; ++i) {
    auto& e = out.events[match->events[i]];
    if (e.after.valid()) {
      if (e.after.home.value == home + 1 && e.after.away.value == away)
        e.beneficiary = match->home.id;
      else if (e.after.away.value == away + 1 && e.after.home.value == home)
        e.beneficiary = match->away.id;
      if (!e.beneficiary.empty())
        e.actor_team = e.type == EventType::kOwnGoal
                           ? (e.beneficiary == match->home.id ? match->away.id
                                                              : match->home.id)
                           : e.beneficiary;
      home = e.after.home.value;
      away = e.after.away.value;
    }
  }
  return true;
}
bool mapMatch(JsonVariantConst row, Snapshot& out, cfg::Provider p,
              uint8_t route, int64_t now, FixtureFilter filter) {
  if (p == cfg::Provider::kOpenLigaDb)
    return mapOpenLigaMatch(row, out, route, now, filter);
  Match m;
  m.provider = p;
  m.route = route;
  if (p == cfg::Provider::kApiFootball) {
    auto fixture = row["fixture"];
    id(m.id, fixture["id"]);
    m.kickoff = instant(fixture["date"]);
    m.home = team(row["teams"]["home"]);
    m.away = team(row["teams"]["away"]);
    m.phase = phase(text(fixture["status"]["short"]));
    m.minute = number(fixture["status"]["elapsed"], 0, 240);
    m.principal = score(row["goals"]);
    m.halftime = score(row["score"]["halftime"]);
    m.regulation = score(row["score"]["fulltime"]);
    m.extra_time = score(row["score"]["extratime"]);
    m.shootout = score(row["score"]["penalty"]);
    m.round_label.assign(text(row["league"]["round"]));
  } else if (p == cfg::Provider::kFootballData) {
    id(m.id, row["id"]);
    m.kickoff = instant(row["utcDate"]);
    m.provider_changed = instant(row["lastUpdated"]);
    m.home = team(row["homeTeam"]);
    m.away = team(row["awayTeam"]);
    m.phase = phase(text(row["status"]));
    auto round = number(row["matchday"], 1, 1000);
    if (round.valid())
      m.round = {static_cast<uint16_t>(round.value), round.source};
    m.round_label.assign(text(row["stage"]));
    auto s = row["score"];
    m.halftime = score(s["halfTime"]);
    m.regulation = score(s["regularTime"]);
    m.shootout = score(s["penalties"]);
    auto extra = score(s["extraTime"]);
    if (m.regulation.valid() && extra.valid())
      m.extra_time = {
          {static_cast<int16_t>(m.regulation.home.value + extra.home.value),
           Source::kProvider},
          {static_cast<int16_t>(m.regulation.away.value + extra.away.value),
           Source::kProvider}};
    const char* duration = text(s["duration"]);
    if (!strcmp(duration, "REGULAR")) {
      m.principal = score(s["fullTime"]);
      m.regulation = m.principal;
    } else
      m.principal = m.extra_time.valid() ? m.extra_time : m.regulation;
  } else {
    id(m.id, row["id"]);
    auto competition = row["competitions"][0];
    m.kickoff = instant(row["date"]);
    const char* state = text(competition["status"]["type"]["state"]);
    const char* status = text(competition["status"]["type"]["name"]);
    m.phase = !strcmp(state, "post") &&
                      competition["status"]["type"]["completed"].as<bool>()
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
    for (JsonVariantConst c :
         competition["competitors"].as<ArduinoJson::JsonArrayConst>()) {
      if (strcmp(text(c["homeAway"]), "home") &&
          strcmp(text(c["homeAway"]), "away"))
        continue;
      const bool home = !strcmp(text(c["homeAway"]), "home");
      (home ? m.home : m.away) =
          team(c["team"], "id", "displayName", "shortDisplayName");
      if (m.phase != Phase::kScheduled)
        (home ? m.principal.home : m.principal.away) = number(
            c["score"].is<ArduinoJson::JsonObjectConst>() ? c["score"]["value"]
                                                          : c["score"],
            0, 99);
      (home ? m.shootout.home : m.shootout.away) =
          number(c["shootoutScore"], 0, 99);
    }
    const char* clock = text(competition["status"]["displayClock"]);
    char* end;
    long minute = strtol(clock, &end, 10);
    if (end != clock && minute >= 0 && minute <= 240 &&
        (m.phase == Phase::kLive || m.phase == Phase::kExtraTime ||
         m.phase == Phase::kPenalties || m.phase == Phase::kHalfTime))
      m.minute = {static_cast<int16_t>(minute), Source::kProvider};
  }
  m.phase_source =
      m.phase == Phase::kUnknown ? Source::kNone : Source::kProvider;
  m.own = std::string_view(filter.team) == m.home.id.data ||
          std::string_view(filter.team) == m.away.id.data;
  if (m.id.empty()) return false;
  if (duplicate(out, m)) return true;
  if (keep(m, now, filter) && out.match_count >= out.matches.size())
    return false;
  auto* match = addMatch(out, m, now, filter);
  if (match != nullptr && match->own && p == cfg::Provider::kEspn) {
    for (JsonVariantConst detail :
         row["competitions"][0]["details"].as<ArduinoJson::JsonArrayConst>())
      mapEvent(detail, out, *match, p);
  }
  return true;
}
bool mapEvent(JsonVariantConst row, Snapshot& out, Match& m, cfg::Provider p) {
  MatchEvent e;
  if (p == cfg::Provider::kApiFootball) {
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
      return true;
    e.minute = number(row["time"]["elapsed"], 0, 240);
    e.added = number(row["time"]["extra"], 0, 120);
    id(e.actor_team, row["team"]["id"]);
    e.player.assign(text(row["player"]["name"]));
    e.detail.assign(detail);
    if (e.type == EventType::kSubstitution) {
      e.player_out = e.player;
      e.player_in.assign(text(row["assist"]["name"]));
    }
  } else if (p == cfg::Provider::kEspn) {
    const char* type = text(row["type"]["text"]);
    e.shootout = row["shootout"].as<bool>();
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
      return true;
    id(e.actor_team, row["team"]["id"]);
    e.player.assign(text(row["athletesInvolved"][0]["displayName"]));
    if (e.type == EventType::kSubstitution) {
      // Keep both names without inferring the direction from array order.
      e.detail.assign(type);
    }
    e.detail.assign(type);
    auto clock = text(row["clock"]["displayValue"]);
    char* end;
    long minute = strtol(clock, &end, 10);
    if (end != clock && minute >= 0 && minute <= 240)
      e.minute = {static_cast<int16_t>(minute), Source::kProvider};
  } else
    return true;
  if (e.type == EventType::kGoal || e.type == EventType::kPenaltyGoal ||
      e.type == EventType::kOwnGoal) {
    if (e.actor_team == m.home.id || e.actor_team == m.away.id)
      e.beneficiary = e.type == EventType::kOwnGoal
                          ? (e.actor_team == m.home.id ? m.away.id : m.home.id)
                          : e.actor_team;
  }
  appendEvent(out, m, e);
  return true;
}
bool mapTable(JsonVariantConst row, Snapshot& out, cfg::Provider p) {
  if (p == cfg::Provider::kApiFootball &&
      row["league"].is<ArduinoJson::JsonObjectConst>()) {
    for (JsonVariantConst group :
         row["league"]["standings"].as<ArduinoJson::JsonArrayConst>())
      for (JsonVariantConst item : group.as<ArduinoJson::JsonArrayConst>())
        if (!mapTable(item, out, p)) return false;
    return true;
  }
  if (p == cfg::Provider::kFootballData &&
      row["table"].is<ArduinoJson::JsonArrayConst>()) {
    if (strcmp(text(row["type"]), "TOTAL")) return true;
    for (JsonVariantConst item : row["table"].as<ArduinoJson::JsonArrayConst>())
      if (!mapTable(item, out, p)) return false;
    return true;
  }
  if (out.table_count >= out.table.size()) return false;
  Standing r;
  if (p == cfg::Provider::kOpenLigaDb) {
    r.team = team(row, "teamInfoId", "teamName", "shortName");
    r.position = {static_cast<int16_t>(out.table_count + 1),
                  Source::kEstimated};
    r.played = number(row["matches"]);
    r.won = number(row["won"]);
    r.drawn = number(row["draw"]);
    r.lost = number(row["lost"]);
    r.goals_for = number(row["goals"]);
    r.goals_against = number(row["opponentGoals"]);
    r.difference = number(row["goalDiff"], -10000, 10000);
    r.points = number(row["points"], -10000, 10000);
  } else if (p == cfg::Provider::kApiFootball) {
    r.team = team(row["team"]);
    r.position = number(row["rank"]);
    r.points = number(row["points"], -10000, 10000);
    r.difference = number(row["goalsDiff"], -10000, 10000);
    auto a = row["all"];
    r.played = number(a["played"]);
    r.won = number(a["win"]);
    r.drawn = number(a["draw"]);
    r.lost = number(a["lose"]);
    r.goals_for = number(a["goals"]["for"]);
    r.goals_against = number(a["goals"]["against"]);
  } else if (p == cfg::Provider::kFootballData) {
    r.team = team(row["team"]);
    r.position = number(row["position"]);
    r.played = number(row["playedGames"]);
    r.won = number(row["won"]);
    r.drawn = number(row["draw"]);
    r.lost = number(row["lost"]);
    r.goals_for = number(row["goalsFor"]);
    r.goals_against = number(row["goalsAgainst"]);
    r.difference = number(row["goalDifference"], -10000, 10000);
    r.points = number(row["points"], -10000, 10000);
  } else {
    r.team = team(row["team"], "id", "displayName", "shortDisplayName");
    for (JsonVariantConst s : row["stats"].as<ArduinoJson::JsonArrayConst>()) {
      const char* n = text(s["name"]);
      auto v = number(s["value"], -10000, 10000);
      if (!strcmp(n, "rank"))
        r.position = v;
      else if (!strcmp(n, "gamesPlayed"))
        r.played = v;
      else if (!strcmp(n, "points"))
        r.points = v;
      else if (!strcmp(n, "wins"))
        r.won = v;
      else if (!strcmp(n, "ties"))
        r.drawn = v;
      else if (!strcmp(n, "losses"))
        r.lost = v;
      else if (!strcmp(n, "pointsFor"))
        r.goals_for = v;
      else if (!strcmp(n, "pointsAgainst"))
        r.goals_against = v;
      else if (!strcmp(n, "pointDifferential"))
        r.difference = v;
    }
  }
  if (r.team.id.empty()) return false;
  out.table[out.table_count++] = r;
  return true;
}
}  // namespace football
