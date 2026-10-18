#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>

#include "civil_time.h"
#include "provider_adapter.h"

namespace football::provider {
namespace {
using json::Kind;
using json::Token;

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
  if (!strcmp(s, "SUSP") || !strcmp(s, "SUSPENDED")) return Phase::kSuspended;
  if (!strcmp(s, "PST") || !strcmp(s, "POSTPONED")) return Phase::kPostponed;
  if (!strcmp(s, "CANC") || !strcmp(s, "CANCELLED")) return Phase::kCancelled;
  if (!strcmp(s, "ABD") || !strcmp(s, "AWD") || !strcmp(s, "ABANDONED") ||
      !strcmp(s, "AWARDED"))
    return Phase::kAbandoned;
  return Phase::kUnknown;
}

bool same(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    auto lower = [](char c) {
      return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    };
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}
// One response workspace, never a JSON subtree. Fixtures need all reported
// events before unordered team fields are known; TOTAL standings need one
// group staged before an unordered type field. The operations are exclusive.
struct Workspace {
  union {
    std::array<Event, cfg::kStoreEvents> events;
    std::array<TableRow, cfg::kStoreTableStageRows> rows;
  };
  explicit Workspace(Operation operation) {
    if (operation == Operation::kTable)
      new (&rows) decltype(rows){};
    else
      new (&events) decltype(events){};
  }
  ~Workspace() {}
};

class Parser {
 public:
  Parser(json::Reader& input, const RequestSpec& spec, const Sink& target)
      : reader(input), request(spec), sink(target) {}
  ParseResult run();

 private:
  json::Reader& reader;
  const RequestSpec& request;
  const Sink& sink;
  std::unique_ptr<Workspace> work;
  ParseResult result;
  char buffer[160]{};
  bool rejected = false, malformed = false, no_memory = false;
  uint8_t group_count = 0;
  std::size_t event_count = 0;
  uint8_t shot_count = 0;
  bool events_cut = false;
  uint32_t row_order = 0;

  bool workspace() {
    if (work) return true;
    if (!no_memory) work.reset(new (std::nothrow) Workspace(request.operation));
    no_memory = !work;
    return !no_memory;
  }
  bool eventDataRequested() const {
    return request.operation == Operation::kFixtures ||
           request.operation == Operation::kOwnFixtures ||
           request.operation == Operation::kSummary;
  }
  bool next(Token& token) { return reader.next(token, buffer, sizeof(buffer)); }
  bool skip(const Token& token) { return reader.skip(token); }
  bool scalar(const Token& token) {
    if (token.kind == Kind::kArray || token.kind == Kind::kObject)
      return skip(token);
    return true;
  }
  template <class F, std::size_t N>
  bool object(const Token& token, const char* const (&names)[N], F consume,
              bool strict = false) {
    if (token.kind != Kind::kObject) {
      malformed = true;
      return scalar(token);
    }
    json::Fields fields(names, N);
    Token value;
    while (next(value)) {
      if (value.kind == Kind::kEndObject) return true;
      if (value.kind != Kind::kKey) return false;
      const int field = fields.accept(value);
      if (field == -2) result.metadata.duplicate = true;
      if (strict && field < 0) result.metadata.invalid = true;
      if (!next(value)) return false;
      if (field < 0) {
        if (!skip(value)) return false;
      } else if (!consume(field, value))
        return false;
    }
    return false;
  }
  template <class F>
  bool array(const Token& token, F consume) {
    if (token.kind != Kind::kArray) {
      malformed = true;
      return scalar(token);
    }
    Token value;
    std::size_t ordinal = 0;
    while (next(value)) {
      if (value.kind == Kind::kEndArray) return true;
      if (!consume(value, ordinal++)) return false;
    }
    return false;
  }
  bool text(const Token& token, std::string_view& out) {
    if (token.kind != Kind::kString || token.contains_nul || token.truncated) {
      out = {};
      return scalar(token) && false;
    }
    out = token.text;
    return true;
  }
  template <std::size_t N>
  bool label(const Token& token, Text<N>& out) {
    if (token.kind != Kind::kString || token.contains_nul) return scalar(token);
    out.assign(token.text);
    return true;
  }
  bool integer(const Token& token, int64_t min, int64_t max, int64_t& out) {
    if ((token.kind != Kind::kNumber && token.kind != Kind::kString) ||
        token.truncated || token.contains_nul || token.text.empty()) {
      scalar(token);
      return false;
    }
    int64_t n = 0;
    if (token.kind == Kind::kNumber) {
      if (!json::exactInteger(token.text, n)) return false;
    } else {
      const auto parsed = std::from_chars(
          token.text.data(), token.text.data() + token.text.size(), n);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != token.text.data() + token.text.size())
        return false;
    }
    if (n < min || n > max) return false;
    out = n;
    return true;
  }
  uint32_t id(const Token& token) {
    if (token.kind == Kind::kString)
      return !token.truncated && !token.contains_nul ? decimalId(token.text)
                                                     : 0;
    int64_t value = 0;
    return integer(token, 1, UINT32_MAX, value) ? static_cast<uint32_t>(value)
                                                : 0;
  }
  uint8_t small(const Token& token, uint8_t maximum = 254) {
    int64_t value = 0;
    return integer(token, 0, maximum, value) ? static_cast<uint8_t>(value)
                                             : kNoValue;
  }
  int16_t number(const Token& token) {
    int64_t value = 0;
    return integer(token, static_cast<int64_t>(kNoNumber) + 1, INT16_MAX, value)
               ? static_cast<int16_t>(value)
               : kNoNumber;
  }
  uint32_t instant(const Token& token) {
    std::string_view value;
    if (!text(token, value)) return 0;
    char timestamp[161]{};
    std::memcpy(timestamp, value.data(), value.size());
    const int64_t at = parseUtc(timestamp);
    return at > 0 && at <= UINT32_MAX ? static_cast<uint32_t>(at) : 0;
  }
  bool equal(const Token& token, std::string_view expected) {
    return !token.contains_nul && !token.truncated &&
           (token.kind == Kind::kString || token.kind == Kind::kNumber) &&
           same(token.text, expected);
  }
  bool boolean(const Token& token) {
    scalar(token);
    return token.kind == Kind::kTrue;
  }
  bool goals(const Token& token, Goals& out, bool olg = false) {
    const char* const keys[] = {olg ? "pointsTeam1" : "home",
                                olg ? "pointsTeam2" : "away"};
    return object(token, keys, [&](int field, const Token& value) {
      (field == 0 ? out.home : out.away) = small(value, 99);
      return true;
    });
  }
  void clock(std::string_view text, uint8_t& minute, uint8_t& added) {
    if (text.empty()) return;
    char copy[64]{};
    const auto n = std::min(text.size(), sizeof(copy) - 1);
    std::memcpy(copy, text.data(), n);
    char* end;
    long value = std::strtol(copy, &end, 10);
    if (end != copy && value >= 0 && value <= 240)
      minute = static_cast<uint8_t>(value);
    if (const char* plus = std::strchr(copy, '+')) {
      value = std::strtol(plus + 1, &end, 10);
      if (end != plus + 1 && value >= 0 && value <= 120)
        added = static_cast<uint8_t>(value);
    }
  }
  bool team(const Token&, TeamInput&, bool olg = false, bool table = false,
            bool espn = false);
  bool emitFixture(Fixture&);
  bool emitRow(TableRow& row) {
    ++result.rows;
    if (!row.team.id) {
      result.metadata.invalid = true;
      return true;
    }
    row.group = group_count ? static_cast<uint8_t>(group_count - 1) : 0;
    if (sink.row && !sink.row(row, sink.context)) {
      rejected = true;
      return false;
    }
    return true;
  }
  bool emitGroup(std::string_view key = {}, std::string_view label = {}) {
    if (group_count == cfg::kStoreTableGroups) {
      result.metadata.invalid = true;
      rejected = true;
      return false;
    }
    TableGroup group;
    group.ordinal = group_count++;
    group.key.assign(key);
    group.label.assign(label);
    if (sink.group && !sink.group(group, sink.context)) {
      rejected = true;
      return false;
    }
    return true;
  }
  void identify(Event& event) {
    if (event.facts.flags & kEventShootout) event.facts.ordinal = shot_count++;
    if (!event.facts.identity) {
      char key[128];
      // A reported attempt's order distinguishes anonymous/same-minute sudden
      // death attempts. Non-shootout identities preserve the existing contract.
      if (event.facts.flags & kEventShootout) {
        std::snprintf(key, sizeof(key), "shot|%lu|%u",
                      static_cast<unsigned long>(event.reported_team),
                      event.facts.ordinal);
      } else
        std::snprintf(
            key, sizeof(key), "%u|%u|%u|%lu|%s|%s|%u|%u",
            unsigned(event.facts.type), event.facts.minute, event.facts.added,
            static_cast<unsigned long>(event.reported_team),
            event.facts.player.data, event.facts.player_in.data,
            unsigned(event.facts.flags & kEventShootout),
            (event.facts.flags & kEventShootout) ? event.facts.ordinal : 0);
      event.facts.identity = fingerprint(key);
    }
  }
  bool add(Event event) {
    if (!eventDataRequested()) return true;
    identify(event);
    if (!workspace()) return false;
    if (event_count < work->events.size())
      work->events[event_count++] = event;
    else {
      events_cut = true;
      result.metadata.invalid = true;
    }
    return true;
  }
  bool olgFixture(const Token&);
  bool olgGoal(const Token&, uint8_t&, uint8_t&);
  bool olgRow(const Token&);
  bool espnFixture(const Token&);
  bool espnCompetition(const Token&, Fixture&);
  bool espnEvent(const Token&, bool summary = false);
  bool espnGroup(const Token&, unsigned depth = 0);
  bool espnRow(const Token&, TableRow&);
  bool espnLeague(const Token&);
  bool afFixture(const Token&);
  bool afTable(const Token&);
  bool afRow(const Token&, TableRow&, Text<31>&);
  bool fdFixture(const Token&);
  bool fdTable(const Token&);
  bool fdRow(const Token&, TableRow&);
  bool root(const Token&);
  bool apiMetadata(int, const Token&);
  bool fdMetadata(int, const Token&);
};

bool Parser::team(const Token& token, TeamInput& out, bool olg, bool table,
                  bool espn) {
  Text<cfg::kTeamNameBytes> short_fallback;
  bool short_provided = false;
  const char* const keys[] = {olg ? (table ? "teamInfoId" : "teamId") : "id",
                              olg    ? "teamName"
                              : espn ? "displayName"
                                     : "name",
                              espn ? "shortDisplayName" : "shortName"};
  const bool ok = object(token, keys, [&](int field, const Token& value) {
    if (field == 0)
      out.id = id(value);
    else if (field == 1)
      label(value, out.name);
    else {
      // Source choice precedes byte adoption: a provided UTF-8 abbreviation
      // can legitimately have no complete codepoint within the3-byte field.
      short_provided = value.kind == Kind::kString && !value.contains_nul &&
                       !value.text.empty();
      label(value, out.short_name);
      label(value, short_fallback);
    }
    return true;
  });
  if (out.name.empty()) out.name = short_fallback;
  if (!short_provided) out.short_name.assign(std::string_view(out.name.data));
  return ok;
}

bool Parser::emitFixture(Fixture& fixture) {
  if (!fixture.scope_matches) result.metadata.invalid = true;
  ++result.rows;
  if (!fixture.facts.id) {
    result.metadata.invalid = true;
    return true;
  }
  fixture.events_truncated = events_cut;
  for (std::size_t i = 0; i < event_count; ++i) {
    auto& event = work->events[i];
    if (event.reported_team)
      event.facts.actor = event.reported_team == fixture.home.id ? Side::kHome
                          : event.reported_team == fixture.away.id
                              ? Side::kAway
                              : Side::kUnknown;
  }
  if (sink.fixture && !sink.fixture(fixture,
                                    work ? std::span<const Event>(
                                               work->events.data(), event_count)
                                         : std::span<const Event>{},
                                    sink.context)) {
    rejected = true;
    return false;
  }
  return true;
}

bool Parser::olgGoal(const Token& token, uint8_t& home, uint8_t& away) {
  Event event;
  Goals after;
  bool own = false, penalty = false;
  const char* const keys[] = {"goalID",    "matchMinute", "goalGetterName",
                              "isOwnGoal", "isPenalty",   "scoreTeam1",
                              "scoreTeam2"};
  const bool ok = object(token, keys, [&](int field, const Token& value) {
    switch (field) {
      case 0: event.facts.identity = id(value); break;
      case 1: event.facts.minute = small(value, 240); break;
      case 2: label(value, event.facts.player); break;
      case 3: own = boolean(value); break;
      case 4: penalty = boolean(value); break;
      case 5: after.home = small(value, 99); break;
      case 6: after.away = small(value, 99); break;
    }
    return true;
  });
  event.facts.type = own       ? EventType::kOwnGoal
                     : penalty ? EventType::kPenaltyGoal
                               : EventType::kGoal;
  if (after.valid()) {
    Side beneficiary =
        after.home == home + 1 && after.away == away   ? Side::kHome
        : after.away == away + 1 && after.home == home ? Side::kAway
                                                       : Side::kUnknown;
    event.facts.actor = !own                         ? beneficiary
                        : beneficiary == Side::kHome ? Side::kAway
                        : beneficiary == Side::kAway ? Side::kHome
                                                     : Side::kUnknown;
    home = after.home;
    away = after.away;
  }
  return ok && add(event);
}

bool Parser::olgFixture(const Token& token) {
  Fixture fixture;
  event_count = shot_count = 0;
  events_cut = false;
  Goals regulation, extra, total;
  bool league = false, season = request.source.season.empty();
  const char* const keys[] = {
      "matchID",        "matchDateTimeUTC", "team1",           "team2",
      "group",          "matchResults",     "matchIsFinished", "goals",
      "leagueShortcut", "leagueSeason"};
  const bool ok = object(token, keys, [&](int field, const Token& value) {
    switch (field) {
      case 0: fixture.facts.id = id(value); break;
      case 1:
        fixture.facts.kickoff = instant(value);
        if (fixture.facts.kickoff) fixture.fields |= kKickoff;
        break;
      case 2: return team(value, fixture.home, true);
      case 3: return team(value, fixture.away, true);
      case 4: {
        const char* const names[] = {"groupOrderID", "groupName"};
        return object(value, names, [&](int f, const Token& v) {
          if (!f) {
            auto round = small(v);
            if (round != kNoValue) {
              fixture.facts.round = round;
              fixture.fields |= kRound;
            }
          } else
            label(v, fixture.round_label);
          return true;
        });
      }
      case 5:
        return array(value, [&](const Token& v, std::size_t) {
          Goals score;
          Text<31> kind;
          const char* const names[] = {"pointsTeam1", "pointsTeam2",
                                       "resultTypeKind"};
          const bool parsed = object(v, names, [&](int f, const Token& item) {
            if (f == 0)
              score.home = small(item, 99);
            else if (f == 1)
              score.away = small(item, 99);
            else
              label(item, kind);
            return true;
          });
          if (std::string_view(kind.data) == "After90Minutes")
            regulation = score;
          else if (std::string_view(kind.data) == "AfterExtraTime")
            extra = score;
          else if (std::string_view(kind.data) == "AfterPenalties")
            total = score;
          return parsed;
        });
      case 6:
        if (value.kind == Kind::kTrue) {
          fixture.facts.phase = Phase::kFinished;
          fixture.fields |= kPhase;
        }
        return scalar(value);
      case 7: {
        if (!eventDataRequested()) return skip(value);
        fixture.carries_events = value.kind == Kind::kArray;
        uint8_t home = 0, away = 0;
        return array(value, [&](const Token& v, std::size_t) {
          return olgGoal(v, home, away);
        });
      }
      case 8: league = equal(value, request.source.competition); break;
      case 9:
        season = request.source.season.empty() ||
                 equal(value, request.source.season);
        if (!value.truncated && !value.contains_nul &&
            (value.kind == Kind::kString || value.kind == Kind::kNumber)) {
          result.metadata.season.assign(value.text);
        }
        break;
    }
    return true;
  });
  fixture.facts.principal = extra.valid() ? extra : regulation;
  if (fixture.facts.principal.valid()) fixture.fields |= kPrincipal;
  if (total.valid() && extra.valid() && total.home >= extra.home &&
      total.away >= extra.away) {
    fixture.facts.shootout = {static_cast<uint8_t>(total.home - extra.home),
                              static_cast<uint8_t>(total.away - extra.away)};
    fixture.fields |= kShootout;
  }
  fixture.source_matches = league && season;
  fixture.scope_matches =
      !request.round || fixture.facts.round == request.round;
  fixture.time_definite = fixture.facts.kickoff != 0;
  result.metadata.round =
      std::max<uint16_t>(result.metadata.round, fixture.facts.round);
  return ok && emitFixture(fixture);
}

bool Parser::olgRow(const Token& token) {
  TableRow row;
  row.facts.position =
      static_cast<uint8_t>(std::min<uint32_t>(++row_order, 254));
  const char* const keys[] = {"teamInfoId", "teamName", "shortName",
                              "matches",    "goalDiff", "points"};
  const bool ok = object(token, keys, [&](int field, const Token& value) {
    switch (field) {
      case 0: row.team.id = id(value); break;
      case 1: label(value, row.team.name); break;
      case 2: label(value, row.team.short_name); break;
      case 3: row.facts.played = small(value); break;
      case 4: row.facts.difference = number(value); break;
      case 5: row.facts.points = number(value); break;
    }
    return true;
  });
  if (row.team.short_name.empty())
    row.team.short_name.assign(std::string_view(row.team.name.data));
  return ok && emitRow(row);
}

bool Parser::espnEvent(const Token& token, bool summary) {
  Event event;
  Text<63> type, technical;
  Text<32> first, second;
  bool own = false, penalty = false, scoring = false;
  const char* const keys[] = {
      "id",       "type",  "ownGoal",          "penaltyKick",  "scoringPlay",
      "shootout", "clock", "athletesInvolved", "participants", "team"};
  const bool ok = object(token, keys, [&](int field, const Token& value) {
    switch (field) {
      case 0: event.facts.identity = id(value); break;
      case 1: {
        const char* const names[] = {"text", "type"};
        return object(value, names, [&](int f, const Token& v) {
          return f ? label(v, technical) : label(v, type);
        });
      }
      case 2: own = boolean(value); break;
      case 3: penalty = boolean(value); break;
      case 4: scoring = boolean(value); break;
      case 5:
        if (boolean(value)) event.facts.flags |= kEventShootout;
        break;
      case 6: {
        const char* const names[] = {"displayValue"};
        return object(value, names, [&](int, const Token& v) {
          std::string_view clock_text;
          if (text(v, clock_text))
            clock(clock_text, event.facts.minute, event.facts.added);
          return true;
        });
      }
      case 7:
        return array(value, [&](const Token& v, std::size_t at) {
          if (at > 1) return skip(v);
          const char* const names[] = {"displayName"};
          return object(v, names, [&](int, const Token& item) {
            return label(item, at ? second : first);
          });
        });
      case 8:
        return array(value, [&](const Token& v, std::size_t at) {
          if (at > 1) return skip(v);
          const char* const names[] = {"athlete"};
          return object(v, names, [&](int, const Token& item) {
            const char* const inner[] = {"displayName"};
            return object(item, inner, [&](int, const Token& n) {
              return label(n, at ? second : first);
            });
          });
        });
      case 9: {
        const char* const names[] = {"id"};
        return object(value, names, [&](int, const Token& v) {
          event.reported_team = id(v);
          return true;
        });
      }
    }
    return true;
  });
  if (!ok) return false;
  if (summary) {
    if (std::string_view(technical.data) != "substitution") return true;
    event.facts.type = EventType::kSubstitution;
    event.facts.player = second;
    event.facts.player_in = first;
    event.facts.flags |= kEventDetail;
    if (!event.facts.identity) {
      identify(event);
    }
    if (sink.event && !sink.event(event, sink.context)) {
      rejected = true;
      return false;
    }
    return true;
  }
  const char* kind = type.data;
  if (own)
    event.facts.type = EventType::kOwnGoal;
  else if (penalty)
    event.facts.type =
        scoring ? EventType::kPenaltyGoal : EventType::kPenaltyMissed;
  else if (scoring || std::strstr(kind, "Goal"))
    event.facts.type = EventType::kGoal;
  else if (std::strstr(kind, "Second Yellow"))
    event.facts.type = EventType::kYellowRed;
  else if (std::strstr(kind, "Yellow"))
    event.facts.type = EventType::kYellow;
  else if (std::strstr(kind, "Red"))
    event.facts.type = EventType::kRed;
  else if (std::strstr(kind, "Substitution"))
    event.facts.type = EventType::kSubstitution;
  else if (std::strstr(kind, "Injury"))
    event.facts.type = EventType::kInjury;
  else if (std::strstr(kind, "VAR"))
    event.facts.type = EventType::kVar;
  else
    return true;
  if (event.facts.type == EventType::kSubstitution && !second.empty()) {
    char both[68];
    std::snprintf(both, sizeof(both), "%s / %s", first.data, second.data);
    event.facts.player.assign(both);
  } else
    event.facts.player = first;
  return add(event);
}

bool Parser::espnCompetition(const Token& token, Fixture& fixture) {
  Text<31> state, status;
  bool completed = false;
  uint8_t reported_period = kNoValue;
  const char* const keys[] = {"status", "competitors", "details", "timeValid"};
  const bool ok = object(token, keys, [&](int field, const Token& value) {
    if (field == 0) {
      const char* const names[] = {"type", "displayClock", "period"};
      return object(value, names, [&](int f, const Token& item) {
        if (f == 0) {
          const char* const inner[] = {"state", "name", "completed"};
          return object(item, inner, [&](int i, const Token& v) {
            if (i == 0)
              label(v, state);
            else if (i == 1)
              label(v, status);
            else
              completed = boolean(v);
            return true;
          });
        }
        if (f == 1) {
          std::string_view clock_text;
          if (text(item, clock_text))
            clock(clock_text, fixture.facts.minute, fixture.facts.added);
        } else
          reported_period = small(item);
        return true;
      });
    }
    if (field == 1)
      return array(value, [&](const Token& v, std::size_t) {
        TeamInput club;
        Text<7> where;
        uint8_t score = kNoValue, shootout = kNoValue;
        const char* const names[] = {"homeAway", "team", "score",
                                     "shootoutScore"};
        const bool parsed = object(v, names, [&](int f, const Token& item) {
          if (f == 0) return label(item, where);
          if (f == 1) return team(item, club, false, false, true);
          if (f == 2 && item.kind == Kind::kObject) {
            const char* const inner[] = {"value"};
            return object(item, inner, [&](int, const Token& n) {
              score = small(n, 99);
              return true;
            });
          }
          (f == 2 ? score : shootout) = small(item, 99);
          return true;
        });
        if (std::string_view(where.data) == "home") {
          fixture.home = club;
          fixture.facts.principal.home = score;
          fixture.facts.shootout.home = shootout;
        } else if (std::string_view(where.data) == "away") {
          fixture.away = club;
          fixture.facts.principal.away = score;
          fixture.facts.shootout.away = shootout;
        }
        return parsed;
      });
    if (field == 2) {
      fixture.carries_events = value.kind == Kind::kArray;
      return array(value,
                   [&](const Token& v, std::size_t) { return espnEvent(v); });
    }
    fixture.time_definite = boolean(value);
    return true;
  });
  auto& phase = fixture.facts.phase;
  phase = std::string_view(state.data) == "post" && completed ? Phase::kFinished
          : std::string_view(state.data) == "in"              ? Phase::kLive
          : std::string_view(state.data) == "pre" ? Phase::kScheduled
                                                  : Phase::kUnknown;
  if (std::strstr(status.data, "SUSPENDED"))
    phase = Phase::kSuspended;
  else if (std::strstr(status.data, "POSTPONED"))
    phase = Phase::kPostponed;
  else if (std::strstr(status.data, "CANCELED") ||
           std::strstr(status.data, "CANCELLED"))
    phase = Phase::kCancelled;
  else if (std::strstr(status.data, "ABANDONED"))
    phase = Phase::kAbandoned;
  else if (std::string_view(status.data) == "STATUS_HALFTIME")
    phase = Phase::kHalfTime;
  else if (std::string_view(state.data) == "in" &&
           std::strstr(status.data, "PEN"))
    phase = Phase::kPenalties;
  else if (std::string_view(state.data) == "in" &&
           std::strstr(status.data, "EXTRA"))
    phase = Phase::kExtraTime;
  if (phase != Phase::kUnknown) fixture.fields |= kPhase;
  if (phase == Phase::kScheduled) fixture.facts.principal = {};
  if (fixture.facts.principal.valid()) fixture.fields |= kPrincipal;
  if (fixture.facts.shootout.valid()) fixture.fields |= kShootout;
  if (!running(phase) && phase != Phase::kSuspended)
    fixture.facts.minute = fixture.facts.added = kNoValue;
  if (fixture.facts.minute != kNoValue) fixture.fields |= kMinute;
  if (fixture.facts.added != kNoValue) fixture.fields |= kAdded;
  if (phase == Phase::kPenalties ||
      (reported_period == 5 && std::strstr(status.data, "PEN")))
    fixture.facts.period = Period::kShootout;
  else if (reported_period == 1)
    fixture.facts.period = Period::kFirstHalf;
  else if (reported_period == 2)
    fixture.facts.period = Period::kSecondHalf;
  if (fixture.facts.period != Period::kUnknown) fixture.fields |= kPeriod;
  return ok;
}

bool Parser::espnFixture(const Token& token) {
  Fixture fixture;
  event_count = shot_count = 0;
  events_cut = false;
  bool season_matches = request.source.season.empty();
  bool league_matches = request.operation != Operation::kOwnFixtures;
  const char* const keys[] = {"id", "date", "season", "competitions", "league"};
  const bool ok = object(token, keys, [&](int field, const Token& value) {
    if (field == 0) {
      fixture.facts.id = id(value);
      return true;
    }
    if (field == 1) {
      fixture.facts.kickoff = instant(value);
      if (fixture.facts.kickoff) fixture.fields |= kKickoff;
      return true;
    }
    if (field == 2) {
      const char* const names[] = {"year"};
      return object(value, names, [&](int, const Token& v) {
        season_matches =
            request.source.season.empty() || equal(v, request.source.season);
        if (!v.contains_nul && !v.truncated &&
            (v.kind == Kind::kNumber || v.kind == Kind::kString))
          result.metadata.season.assign(v.text);
        return true;
      });
    }
    if (field == 4) {
      const char* const names[] = {"slug"};
      return object(value, names, [&](int, const Token& v) {
        league_matches = equal(v, request.source.competition);
        return true;
      });
    }
    return array(value, [&](const Token& v, std::size_t at) {
      return at ? skip(v) : espnCompetition(v, fixture);
    });
  });
  fixture.source_matches =
      season_matches && league_matches &&
      (request.operation != Operation::kOwnFixtures ||
       (request.team &&
        (fixture.home.id == request.team || fixture.away.id == request.team)));
  if (request.operation == Operation::kFixtures && request.from &&
      fixture.facts.kickoff) {
    const uint64_t from = uint64_t{request.from / timekeeping::kSecondsPerDay} *
                          timekeeping::kSecondsPerDay;
    const uint64_t to = uint64_t{(request.to ? request.to : request.from) /
                                 timekeeping::kSecondsPerDay} *
                            timekeeping::kSecondsPerDay +
                        timekeeping::kSecondsPerDay;
    fixture.scope_matches =
        fixture.facts.kickoff >= from && fixture.facts.kickoff < to;
  }
  return ok && emitFixture(fixture);
}

bool Parser::espnRow(const Token& token, TableRow& row) {
  const char* const keys[] = {"team", "stats"};
  return object(token, keys, [&](int field, const Token& value) {
    if (!field) return team(value, row.team, false, false, true);
    return array(value, [&](const Token& v, std::size_t) {
      Text<31> name;
      int16_t score = kNoNumber;
      const char* const names[] = {"name", "value"};
      const bool ok = object(v, names, [&](int f, const Token& item) {
        if (!f) return label(item, name);
        score = number(item);
        return true;
      });
      if (std::string_view(name.data) == "rank" && score >= 0 &&
          score < kNoValue)
        row.facts.position = static_cast<uint8_t>(score);
      else if (std::string_view(name.data) == "gamesPlayed" && score >= 0 &&
               score < kNoValue)
        row.facts.played = static_cast<uint8_t>(score);
      else if (std::string_view(name.data) == "points")
        row.facts.points = score;
      else if (std::string_view(name.data) == "pointDifferential")
        row.facts.difference = score;
      return ok;
    });
  });
}

bool Parser::espnGroup(const Token& token, unsigned depth) {
  if (depth > cfg::kJsonDepth) {
    malformed = true;
    return skip(token);
  }
  Text<31> key, title;
  bool emitted = false;
  uint8_t ordinal = kNoIndex;
  const char* const keys[] = {"id", "name", "abbreviation", "standings",
                              "children"};
  const bool parsed = object(token, keys, [&](int field, const Token& value) {
    if (field == 0) {
      if (!value.contains_nul && !value.truncated) key.assign(value.text);
      return scalar(value);
    }
    if (field == 1 || field == 2) return label(value, title);
    if (field == 4)
      return array(value, [&](const Token& v, std::size_t) {
        return espnGroup(v, depth + 1);
      });
    const char* const names[] = {"entries"};
    return object(value, names, [&](int, const Token& v) {
      result.primary_seen = true;
      if (!emitted) {
        ordinal = group_count;
        if (!emitGroup(key.data, title.data)) return false;
        emitted = true;
      }
      return array(v, [&](const Token& item, std::size_t) {
        TableRow row;
        return espnRow(item, row) && emitRow(row);
      });
    });
  });
  if (parsed && emitted && sink.group) {
    TableGroup group;
    group.ordinal = ordinal;
    group.key = key;
    group.label = title;
    if (!sink.group(group, sink.context)) {
      rejected = true;
      return false;
    }
  }
  return parsed;
}

bool Parser::espnLeague(const Token& token) {
  bool league = false, season = request.source.season.empty(), day = false,
       whitelist = false, calendar_bad = false;
  uint32_t first = 0, last = 0;
  const char* const keys[] = {"slug",
                              "season",
                              "calendarType",
                              "calendarIsWhitelist",
                              "calendarStartDate",
                              "calendarEndDate",
                              "calendar"};
  const bool ok = object(token, keys, [&](int field, const Token& value) {
    switch (field) {
      case 0: league = equal(value, request.source.competition); break;
      case 1: {
        const char* const names[] = {"year"};
        return object(value, names, [&](int, const Token& v) {
          season =
              request.source.season.empty() || equal(v, request.source.season);
          return true;
        });
      }
      case 2: day = equal(value, "day"); break;
      case 3: whitelist = boolean(value); break;
      case 4: result.metadata.from = instant(value); break;
      case 5: result.metadata.to = instant(value); break;
      case 6:
        return array(value, [&](const Token& v, std::size_t) {
          const auto at = instant(v);
          if (!at) {
            calendar_bad = true;
            return true;
          }
          first = first ? std::min(first, at) : at;
          last = std::max(last, at);
          ++result.metadata.calendar_count;
          if (at <= request.from)
            result.metadata.previous_day =
                std::max(result.metadata.previous_day, at);
          else if (!result.metadata.next_day || at < result.metadata.next_day)
            result.metadata.next_day = at;
          return true;
        });
    }
    return true;
  });
  result.metadata.source_matches = league && season;
  if (day && calendar_bad) result.metadata.invalid = true;
  result.metadata.calendar_verified =
      day && whitelist && !calendar_bad && first && result.metadata.from &&
      result.metadata.to >= result.metadata.from &&
      first >= result.metadata.from && last <= result.metadata.to;
  // A scoreboard's league/season identifies its requested fixture day.
  // Cup scoreboards carry grouped calendar entries instead of a day whitelist;
  // that unrelated calendar shape cannot invalidate their complete fixtures.
  // Any absence proof still requires calendar_verified independently.
  result.metadata.metadata_complete = result.metadata.source_matches;
  return ok;
}

bool Parser::afFixture(const Token& token) {
  Fixture fixture;
  event_count = shot_count = 0;
  events_cut = false;
  bool league = false, season = request.source.season.empty();
  Text<15> status;
  const char* const keys[] = {"fixture", "league", "teams", "goals", "score"};
  const bool ok = object(token, keys, [&](int field, const Token& value) {
    if (field == 0) {
      const char* const names[] = {"id", "date", "status"};
      return object(value, names, [&](int f, const Token& v) {
        if (!f)
          fixture.facts.id = id(v);
        else if (f == 1) {
          fixture.facts.kickoff = instant(v);
          if (fixture.facts.kickoff) fixture.fields |= kKickoff;
        } else {
          const char* const inner[] = {"short", "elapsed", "extra"};
          return object(v, inner, [&](int i, const Token& n) {
            if (!i)
              label(n, status);
            else if (i == 1)
              fixture.facts.minute = small(n, 240);
            else
              fixture.facts.added = small(n, 120);
            return true;
          });
        }
        return true;
      });
    }
    if (field == 1) {
      const char* const names[] = {"id", "season", "round"};
      return object(value, names, [&](int f, const Token& v) {
        if (!f)
          league = equal(v, request.source.competition);
        else if (f == 1)
          season =
              request.source.season.empty() || equal(v, request.source.season);
        else
          label(v, fixture.round_label);
        return true;
      });
    }
    if (field == 2) {
      const char* const names[] = {"home", "away"};
      return object(value, names, [&](int f, const Token& v) {
        return team(v, f ? fixture.away : fixture.home);
      });
    }
    if (field == 3) return goals(value, fixture.facts.principal);
    const char* const names[] = {"penalty"};
    return object(value, names, [&](int, const Token& v) {
      return goals(v, fixture.facts.shootout);
    });
  });
  fixture.facts.phase = statusPhase(status.data);
  if (fixture.facts.phase != Phase::kUnknown) fixture.fields |= kPhase;
  if (std::string_view(status.data) == "1H" ||
      std::string_view(status.data) == "HT")
    fixture.facts.period = Period::kFirstHalf;
  else if (std::string_view(status.data) == "2H")
    fixture.facts.period = Period::kSecondHalf;
  else if (fixture.facts.phase == Phase::kPenalties)
    fixture.facts.period = Period::kShootout;
  if (fixture.facts.period != Period::kUnknown) fixture.fields |= kPeriod;
  if (fixture.facts.minute != kNoValue) fixture.fields |= kMinute;
  if (fixture.facts.added != kNoValue) fixture.fields |= kAdded;
  if (fixture.facts.principal.valid()) fixture.fields |= kPrincipal;
  if (fixture.facts.shootout.valid()) fixture.fields |= kShootout;
  fixture.time_definite = fixture.facts.kickoff && !status.empty() &&
                          std::string_view(status.data) != "TBD" &&
                          std::string_view(status.data) != "PST";
  fixture.source_matches = league && season;
  return ok && emitFixture(fixture);
}

bool Parser::afRow(const Token& token, TableRow& row, Text<31>& caption) {
  const char* const keys[] = {"rank",      "team", "points",
                              "goalsDiff", "all",  "group"};
  return object(token, keys, [&](int field, const Token& value) {
    if (!field)
      row.facts.position = small(value);
    else if (field == 1)
      return team(value, row.team);
    else if (field == 2)
      row.facts.points = number(value);
    else if (field == 3)
      row.facts.difference = number(value);
    else if (field == 4) {
      const char* const names[] = {"played"};
      return object(value, names, [&](int, const Token& v) {
        row.facts.played = small(v);
        return true;
      });
    } else
      return label(value, caption);
    return true;
  });
}
bool Parser::afTable(const Token& token) {
  const char* const keys[] = {"league"};
  return object(token, keys, [&](int, const Token& value) {
    const char* const names[] = {"standings"};
    return object(value, names, [&](int, const Token& v) {
      result.primary_seen = true;
      return array(v, [&](const Token& group, std::size_t ordinal) {
        Text<31> caption;
        std::size_t count = 0;
        bool overflow = false;
        const bool parsed = array(group, [&](const Token& item, std::size_t) {
          if (!workspace()) return false;
          if (count == work->rows.size()) {
            overflow = true;
            return skip(item);
          }
          auto& row = work->rows[count++];
          row = {};
          Text<31> row_caption;
          const bool ok = afRow(item, row, row_caption);
          if (caption.empty())
            caption = row_caption;
          else if (!row_caption.empty() && caption != row_caption)
            result.metadata.invalid = true;
          return ok;
        });
        if (!parsed) return false;
        if (overflow) {
          result.metadata.invalid = true;
          rejected = true;
          return false;
        }
        char key[12];
        std::snprintf(key, sizeof(key), "%u", static_cast<unsigned>(ordinal));
        if (!emitGroup(key, caption.data)) return false;
        for (std::size_t at = 0; at < count; ++at)
          if (!emitRow(work->rows[at])) return false;
        return true;
      });
    });
  });
}

bool Parser::fdFixture(const Token& token) {
  Fixture fixture;
  event_count = shot_count = 0;
  events_cut = false;
  Text<31> status, duration;
  Goals full, regulation, extra;
  bool league = false, season = request.source.season.empty();
  const char* const keys[] = {"id",          "utcDate",  "status",   "matchday",
                              "stage",       "homeTeam", "awayTeam", "score",
                              "competition", "season"};
  const bool ok = object(token, keys, [&](int field, const Token& value) {
    switch (field) {
      case 0: fixture.facts.id = id(value); break;
      case 1:
        fixture.facts.kickoff = instant(value);
        if (fixture.facts.kickoff) fixture.fields |= kKickoff;
        break;
      case 2: label(value, status); break;
      case 3: {
        const auto round = small(value);
        if (round != kNoValue) {
          fixture.facts.round = round;
          fixture.fields |= kRound;
        }
        break;
      }
      case 4: label(value, fixture.round_label); break;
      case 5: return team(value, fixture.home);
      case 6: return team(value, fixture.away);
      case 7: {
        const char* const names[] = {"duration", "fullTime", "regularTime",
                                     "extraTime", "penalties"};
        return object(value, names, [&](int f, const Token& v) {
          if (!f) return label(v, duration);
          return goals(v, f == 1   ? full
                          : f == 2 ? regulation
                          : f == 3 ? extra
                                   : fixture.facts.shootout);
        });
      }
      case 8: {
        const char* const names[] = {"id", "code"};
        return object(value, names, [&](int, const Token& v) {
          league |= equal(v, request.source.competition);
          return scalar(v);
        });
      }
      case 9: {
        const char* const names[] = {"startDate"};
        return object(value, names, [&](int, const Token& v) {
          season = request.source.season.empty() ||
                   (!v.contains_nul && v.kind == Kind::kString &&
                    v.text.size() >= 4 &&
                    v.text.substr(0, 4) == request.source.season);
          return scalar(v);
        });
      }
    }
    return true;
  });
  fixture.facts.phase = statusPhase(status.data);
  if (fixture.facts.phase != Phase::kUnknown) fixture.fields |= kPhase;
  fixture.facts.principal = regulation;
  if (std::string_view(duration.data) == "REGULAR")
    fixture.facts.principal = full;
  else if (regulation.valid() && extra.valid() &&
           regulation.home + extra.home <= 99 &&
           regulation.away + extra.away <= 99)
    fixture.facts.principal = {
        static_cast<uint8_t>(regulation.home + extra.home),
        static_cast<uint8_t>(regulation.away + extra.away)};
  if (fixture.facts.principal.valid()) fixture.fields |= kPrincipal;
  if (fixture.facts.shootout.valid()) fixture.fields |= kShootout;
  fixture.source_matches = league && season;
  fixture.time_definite = fixture.facts.kickoff && !status.empty() &&
                          std::string_view(status.data) != "SCHEDULED" &&
                          std::string_view(status.data) != "POSTPONED";
  return ok && emitFixture(fixture);
}

bool Parser::fdRow(const Token& token, TableRow& row) {
  const char* const keys[] = {"position", "team", "playedGames", "points",
                              "goalDifference"};
  return object(token, keys, [&](int field, const Token& value) {
    if (!field)
      row.facts.position = small(value);
    else if (field == 1)
      return team(value, row.team);
    else if (field == 2)
      row.facts.played = small(value);
    else if (field == 3)
      row.facts.points = number(value);
    else
      row.facts.difference = number(value);
    return true;
  });
}

bool Parser::fdTable(const Token& token) {
  Text<31> type, key, title;
  std::size_t count = 0;
  bool overflow = false, seen = false;
  const char* const keys[] = {"type", "stage", "group", "table"};
  const bool ok = object(token, keys, [&](int field, const Token& value) {
    if (!field) return label(value, type);
    if (field == 1) return label(value, title);
    if (field == 2) return label(value, key);
    seen = value.kind == Kind::kArray;
    return array(value, [&](const Token& v, std::size_t) {
      if (!workspace()) return false;
      if (count == work->rows.size()) {
        overflow = true;
        return skip(v);
      }
      auto& row = work->rows[count++];
      row = {};
      return fdRow(v, row);
    });
  });
  if (std::string_view(type.data) != "TOTAL") return ok;
  if (!seen) {
    result.metadata.invalid = true;
    return ok;
  }
  result.primary_seen = true;
  if (overflow) {
    result.metadata.invalid = true;
    rejected = true;
    return false;
  }
  if (!emitGroup(key.data, key.empty() ? title.data : key.data)) return false;
  for (std::size_t at = 0; at < count; ++at)
    if (!emitRow(work->rows[at])) return false;
  return ok;
}

bool Parser::apiMetadata(int field, const Token& value) {
  auto& meta = result.metadata;
  if (field == 1) {
    const bool full_season = !request.from && !request.to;
    bool league = false, season = false, from = full_season, to = full_season;
    const char* const keys[] = {"league", "season", "from", "to"};
    const bool ok = object(
        value, keys,
        [&](int f, const Token& v) {
          if (!f)
            league = equal(v, request.source.competition);
          else if (f == 1) {
            season = equal(v, request.source.season);
            if (!v.contains_nul && !v.truncated) meta.season.assign(v.text);
          } else {
            if (full_season) {
              meta.invalid = true;
              return scalar(v);
            }
            const uint32_t at = instant(v);
            // Provider filters are civil UTC dates, not full timestamps.
            char expected[11]{};
            const time_t raw = f == 2 ? request.from : request.to;
            std::tm tm{};
            if (gmtime_r(&raw, &tm))
              strftime(expected, sizeof(expected), "%Y-%m-%d", &tm);
            const bool matches = equal(v, expected);
            (f == 2 ? from : to) = matches;
            if (at) (f == 2 ? meta.from : meta.to) = at;
          }
          return true;
        },
        true);
    meta.source_matches = league && season;
    meta.metadata_complete = league && season && from && to;
    return ok;
  }
  if (field == 2) {
    const char* const keys[] = {"current", "total"};
    return object(
        value, keys,
        [&](int f, const Token& v) {
          int64_t n = 0;
          if (!integer(v, 1, UINT16_MAX, n))
            meta.invalid = true;
          else
            (f ? meta.total_pages : meta.current_page) =
                static_cast<uint16_t>(n);
          return true;
        },
        true);
  }
  if (field == 3) {
    int64_t n = 0;
    meta.expected_rows_present = integer(value, 0, UINT32_MAX, n);
    if (meta.expected_rows_present)
      meta.expected_rows = static_cast<uint32_t>(n);
    return true;
  }
  // errors is required, and only an empty object/array is successful.
  bool empty = true;
  if (value.kind == Kind::kArray) {
    if (!array(value, [&](const Token& v, std::size_t) {
          empty = false;
          return skip(v);
        }))
      return false;
  } else if (value.kind == Kind::kObject) {
    Token v;
    while (next(v)) {
      if (v.kind == Kind::kEndObject) break;
      empty = false;
      if (v.kind != Kind::kKey || !next(v) || !skip(v)) return false;
    }
  } else {
    empty = false;
    if (!scalar(value)) return false;
  }
  if (!empty) meta.invalid = true;
  return true;
}

bool Parser::fdMetadata(int field, const Token& value) {
  auto& meta = result.metadata;
  if (field == 1) {
    const bool full_season = !request.from && !request.to;
    bool from = full_season, to = full_season, season = false;
    const char* const keys[] = {"dateFrom", "dateTo", "season", "permission"};
    const bool ok = object(
        value, keys,
        [&](int f, const Token& v) {
          if (f == 2) {
            season = request.source.season.empty() ||
                     equal(v, request.source.season);
            if (!v.contains_nul && !v.truncated) meta.season.assign(v.text);
          } else if (f < 2) {
            if (full_season) {
              meta.invalid = true;
              return scalar(v);
            }
            const uint64_t second = f ? uint64_t{request.to} + 1 : request.from;
            const time_t raw = second;
            std::tm tm{};
            char expected[11]{};
            if (gmtime_r(&raw, &tm))
              strftime(expected, sizeof(expected), "%Y-%m-%d", &tm);
            (f ? to : from) = equal(v, expected);
          } else
            return scalar(v);
          return true;
        },
        true);
    meta.metadata_complete = from && to && season;
    return ok;
  }
  if (field == 2) {
    const char* const keys[] = {"id", "code"};
    return object(value, keys, [&](int, const Token& v) {
      meta.source_matches |= equal(v, request.source.competition);
      return scalar(v);
    });
  }
  const char* const keys[] = {"count"};
  return object(value, keys, [&](int, const Token& v) {
    int64_t n = 0;
    meta.expected_rows_present = integer(v, 0, UINT32_MAX, n);
    if (meta.expected_rows_present)
      meta.expected_rows = static_cast<uint32_t>(n);
    return true;
  });
}

bool Parser::root(const Token& token) {
  const auto operation = request.operation;
  const bool table = operation == Operation::kTable;
  if (request.source.provider == cfg::Provider::kOpenLigaDb) {
    if (operation == Operation::kLastChange) {
      result.primary_seen = token.kind == Kind::kString &&
                            !token.contains_nul && !token.truncated;
      if (result.primary_seen && sink.text &&
          !sink.text(token.text, sink.context)) {
        rejected = true;
        return false;
      }
      return scalar(token);
    }
    result.primary_seen = token.kind == Kind::kArray;
    if (table && !emitGroup()) return false;
    const bool ok = array(token, [&](const Token& value, std::size_t) {
      return table ? olgRow(value) : olgFixture(value);
    });
    result.metadata.metadata_complete = true;
    result.metadata.source_matches = true;
    return ok;
  }
  if (request.source.provider == cfg::Provider::kEspn) {
    if (operation == Operation::kOwnFixtures) {
      uint8_t metadata_seen = 0;
      bool season = false, team_id = false;
      const char* const keys[] = {"events", "requestedSeason", "team"};
      const bool ok = object(token, keys, [&](int field, const Token& value) {
        if (!field) {
          result.primary_seen = value.kind == Kind::kArray;
          return array(value, [&](const Token& v, std::size_t) {
            return espnFixture(v);
          });
        }
        metadata_seen |= 1u << field;
        if (field == 1) {
          const char* const names[] = {"year"};
          return object(value, names, [&](int, const Token& v) {
            season = request.source.season.empty() ||
                     equal(v, request.source.season);
            if (!v.contains_nul && !v.truncated)
              result.metadata.season.assign(v.text);
            return true;
          });
        }
        const char* const names[] = {"id"};
        return object(value, names, [&](int, const Token& v) {
          team_id = id(v) == request.team;
          return true;
        });
      });
      result.metadata.source_matches = season && team_id;
      result.metadata.metadata_complete =
          metadata_seen == 6 && season && team_id;
      if (!result.metadata.metadata_complete) result.metadata.invalid = true;
      return ok;
    }
    if (operation == Operation::kCalendarDays ||
        operation == Operation::kSeasonCalendar) {
      bool type = false, season = false;
      const auto dates = [&](const Token& value) {
        if (result.primary_seen) result.metadata.duplicate = true;
        result.primary_seen = value.kind == Kind::kArray;
        return array(value, [&](const Token& v, std::size_t) {
          const auto day = instant(v);
          if (!day) {
            result.metadata.invalid = true;
            return true;
          }
          ++result.rows;
          ++result.metadata.calendar_count;
          if (day <= request.from)
            result.metadata.previous_day =
                std::max(result.metadata.previous_day, day);
          else if (!result.metadata.next_day || day < result.metadata.next_day)
            result.metadata.next_day = day;
          if (sink.day && !sink.day(day, sink.context)) {
            rejected = true;
            return false;
          }
          return true;
        });
      };
      const char* const keys[] = {"eventDate", "dates",  "startDate", "endDate",
                                  "type",      "season", "sections"};
      const bool ok = object(token, keys, [&](int field, const Token& value) {
        if (field == 1) return dates(value);
        if (field == 2) {
          result.metadata.from = instant(value);
          return true;
        }
        if (field == 3) {
          result.metadata.to = instant(value);
          return true;
        }
        if (field == 4) {
          std::string_view name;
          if (text(value, name)) type = name == "day" || name == "list";
          return true;
        }
        if (field == 5) {
          const char* const names[] = {"$ref"};
          return object(value, names, [&](int, const Token& ref) {
            std::string_view address;
            if (!text(ref, address)) return false;
            address = address.substr(0, address.find('?'));
            const auto split = address.rfind('/');
            if (split == std::string_view::npos) return true;
            const auto year = address.substr(split + 1);
            char expected[160];
            const int size = std::snprintf(
                expected, sizeof(expected),
                "sports.core.api.espn.com/v2/sports/soccer/leagues/%.*s/"
                "seasons",
                static_cast<int>(request.source.competition.size()),
                request.source.competition.data());
            auto prefix = address.substr(0, split);
            if (prefix.starts_with("https://"))
              prefix.remove_prefix(8);
            else if (prefix.starts_with("http://"))
              prefix.remove_prefix(7);
            season = size > 0 &&
                     static_cast<std::size_t>(size) < sizeof(expected) &&
                     prefix == std::string_view(expected) && year.size() == 4 &&
                     decimalId(year) &&
                     (request.source.season.empty() ||
                      year == request.source.season);
            if (season) result.metadata.season.assign(year);
            return true;
          });
        }
        if (field == 6)
          return array(value, [&](const Token& section, std::size_t) {
            const char* const names[] = {"entries"};
            return object(section, names, [&](int, const Token& entries) {
              return array(entries, [&](const Token& entry, std::size_t) {
                CalendarRound round;
                const char* const fields[] = {"label", "value", "startDate",
                                              "endDate"};
                const bool parsed =
                    object(entry, fields, [&](int field, const Token& v) {
                      if (field == 0) return label(v, round.label);
                      if (field == 1) {
                        const auto value = small(v);
                        round.number = value == kNoValue ? 0 : value;
                        return true;
                      }
                      (field == 2 ? round.from : round.to) = instant(v);
                      return true;
                    });
                if (!parsed) return false;
                if (!round.from || round.to < round.from ||
                    round.label.empty()) {
                  result.metadata.invalid = true;
                  return true;
                }
                if (sink.calendar_round &&
                    !sink.calendar_round(round, sink.context)) {
                  rejected = true;
                  return false;
                }
                return true;
              });
            });
          });
        const char* const names[] = {"dates"};
        return object(value, names,
                      [&](int, const Token& v) { return dates(v); });
      });
      result.metadata.source_matches = season;
      result.metadata.calendar_verified =
          type && season && result.metadata.from &&
          result.metadata.to >= result.metadata.from;
      result.metadata.metadata_complete = result.metadata.calendar_verified;
      if (!request.source.season.empty() && !season)
        result.metadata.invalid = true;
      return ok;
    }
    if (operation == Operation::kSummary) {
      const char* const keys[] = {"keyEvents"};
      return object(token, keys, [&](int, const Token& value) {
        result.primary_seen = value.kind == Kind::kArray;
        return array(value, [&](const Token& v, std::size_t) {
          ++result.rows;
          return espnEvent(v, true);
        });
      });
    }
    if (table) return espnGroup(token);
    const char* const keys[] = {"events", "leagues"};
    return object(token, keys, [&](int field, const Token& value) {
      if (!field) {
        result.primary_seen = value.kind == Kind::kArray;
        return array(
            value, [&](const Token& v, std::size_t) { return espnFixture(v); });
      }
      std::size_t count = 0;
      const bool ok = array(value, [&](const Token& v, std::size_t) {
        ++count;
        return espnLeague(v);
      });
      if (count != 1) {
        result.metadata.invalid = true;
        result.metadata.metadata_complete = false;
      }
      return ok;
    });
  }
  if (request.source.provider == cfg::Provider::kApiFootball) {
    const char* const keys[] = {"response", "parameters", "paging", "results",
                                "errors"};
    uint8_t metadata_seen = 0;
    const bool ok = object(token, keys, [&](int field, const Token& value) {
      if (field) {
        metadata_seen |= 1u << field;
        return apiMetadata(field, value);
      }
      if (table) {
        // Explicit root response [] is an authoritative empty table.
        result.primary_seen = value.kind == Kind::kArray;
        return array(value,
                     [&](const Token& v, std::size_t) { return afTable(v); });
      }
      result.primary_seen = value.kind == Kind::kArray;
      if (operation == Operation::kCurrentSeason)
        return array(value, [&](const Token& v, std::size_t) {
          const char* const names[] = {"seasons"};
          return object(v, names, [&](int, const Token& item) {
            return array(item, [&](const Token& season, std::size_t) {
              Id year;
              bool current = false;
              const char* const inner[] = {"year", "current"};
              const bool parsed =
                  object(season, inner, [&](int f, const Token& n) {
                    if (!f && !n.truncated && !n.contains_nul)
                      year.assign(n.text);
                    else if (f == 1)
                      current = boolean(n);
                    return true;
                  });
              if (current) result.metadata.season = year;
              return parsed;
            });
          });
        });
      return array(value,
                   [&](const Token& v, std::size_t) { return afFixture(v); });
    });
    if (operation == Operation::kFixtures)
      result.metadata.metadata_complete &=
          metadata_seen == 30 && result.metadata.current_page == 1 &&
          result.metadata.total_pages == 1 &&
          result.metadata.expected_rows_present &&
          result.metadata.expected_rows == result.rows;
    return ok;
  }
  const char* const keys[] = {table ? "standings" : "matches", "filters",
                              "competition", "resultSet"};
  uint8_t metadata_seen = 0;
  const bool ok = object(token, keys, [&](int field, const Token& value) {
    if (field) {
      metadata_seen |= 1u << field;
      return fdMetadata(field, value);
    }
    result.primary_seen = value.kind == Kind::kArray;
    if (table)
      return array(value,
                   [&](const Token& v, std::size_t) { return fdTable(v); });
    return array(value,
                 [&](const Token& v, std::size_t) { return fdFixture(v); });
  });
  if (!table)
    result.metadata.metadata_complete &=
        metadata_seen == 14 && result.metadata.source_matches &&
        result.metadata.expected_rows_present &&
        result.metadata.expected_rows == result.rows;
  return ok;
}

ParseResult Parser::run() {
  Token token;
  if (!next(token) || !root(token) || !reader.finish()) {
    result.status = no_memory  ? Status::kNoMemory
                    : rejected ? Status::kRejected
                               : Status::kBadResponse;
    return result;
  }
  result.document_complete = !result.metadata.duplicate && !malformed;
  if (!result.document_complete || result.metadata.invalid)
    result.status = Status::kBadResponse;
  else if (!result.primary_seen)
    result.status = Status::kMissing;
  else
    result.status = Status::kOk;
  return result;
}
}  // namespace

ParseResult readResponse(json::Reader& reader, const RequestSpec& request,
                         const Sink& sink) {
  if (request.operation == Operation::kCompetitions ||
      request.operation == Operation::kTeams ||
      request.operation == Operation::kOriginalNames)
    return readSelectionResponse(reader, request, sink);
  Parser parser(reader, request, sink);
  return parser.run();
}
}  // namespace football::provider
