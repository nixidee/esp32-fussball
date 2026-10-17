#include "provider_coverage.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

namespace football::coverage {
namespace {
using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonVariantConst;
constexpr uint32_t kDay = 86400;
constexpr const char* kParameters[] = {"parameters"};
constexpr const char* kParameterLeague[] = {"parameters", "league"};
constexpr const char* kParameterSeason[] = {"parameters", "season"};
constexpr const char* kParameterFrom[] = {"parameters", "from"};
constexpr const char* kParameterTo[] = {"parameters", "to"};
constexpr const char* kPaging[] = {"paging"};
constexpr const char* kCurrentPage[] = {"paging", "current"};
constexpr const char* kTotalPages[] = {"paging", "total"};
constexpr const char* kResults[] = {"results"};
constexpr const char* kErrors[] = {"errors"};
constexpr const char* kFilters[] = {"filters"};
constexpr const char* kFilterFrom[] = {"filters", "dateFrom"};
constexpr const char* kFilterTo[] = {"filters", "dateTo"};
constexpr const char* kFilterSeason[] = {"filters", "season"};
constexpr const char* kPermission[] = {"filters", "permission"};
constexpr const char* kCompetition[] = {"competition"};
constexpr const char* kCompetitionId[] = {"competition", "id"};
constexpr const char* kCompetitionCode[] = {"competition", "code"};
constexpr const char* kResultSet[] = {"resultSet"};
constexpr const char* kResultCount[] = {"resultSet", "count"};
constexpr const char* kLeagues[] = {"leagues"};
constexpr const char* kLeague[] = {"leagues", "slug"};
constexpr const char* kYear[] = {"leagues", "season", "year"};
constexpr const char* kType[] = {"leagues", "calendarType"};
constexpr const char* kWhitelist[] = {"leagues", "calendarIsWhitelist"};
constexpr const char* kStart[] = {"leagues", "calendarStartDate"};
constexpr const char* kEnd[] = {"leagues", "calendarEndDate"};
constexpr const char* kCalendar[] = {"leagues", "calendar"};
const JsonField kApiFields[] = {
    {kParameters, 1, JsonDelivery::kObjectCount, JsonValueKind::kNumber,
     nullptr, 0},
    {kParameterLeague, 2, JsonDelivery::kValue, JsonValueKind::kAny, nullptr,
     1},
    {kParameterSeason, 2, JsonDelivery::kValue, JsonValueKind::kAny, nullptr,
     2},
    {kParameterFrom, 2, JsonDelivery::kValue, JsonValueKind::kString, nullptr,
     3},
    {kParameterTo, 2, JsonDelivery::kValue, JsonValueKind::kString, nullptr, 4},
    {kPaging, 1, JsonDelivery::kObjectCount, JsonValueKind::kNumber, nullptr,
     5},
    {kCurrentPage, 2, JsonDelivery::kValue, JsonValueKind::kNumber, nullptr, 6},
    {kTotalPages, 2, JsonDelivery::kValue, JsonValueKind::kNumber, nullptr, 7},
    {kResults, 1, JsonDelivery::kValue, JsonValueKind::kNumber, nullptr, 8},
    {kErrors, 1, JsonDelivery::kValue, JsonValueKind::kAny, nullptr, 9}};
const JsonField kFootballDataFields[] = {
    {kFilters, 1, JsonDelivery::kObjectCount, JsonValueKind::kNumber, nullptr,
     0},
    {kFilterFrom, 2, JsonDelivery::kValue, JsonValueKind::kString, nullptr, 1},
    {kFilterTo, 2, JsonDelivery::kValue, JsonValueKind::kString, nullptr, 2},
    {kFilterSeason, 2, JsonDelivery::kValue, JsonValueKind::kAny, nullptr, 3},
    {kPermission, 2, JsonDelivery::kValue, JsonValueKind::kString, nullptr, 4},
    {kCompetition, 1, JsonDelivery::kObjectCount, JsonValueKind::kNumber,
     nullptr, 5},
    {kCompetitionId, 2, JsonDelivery::kValue, JsonValueKind::kAny, nullptr, 6},
    {kCompetitionCode, 2, JsonDelivery::kValue, JsonValueKind::kAny, nullptr,
     7},
    {kResultSet, 1, JsonDelivery::kObjectCount, JsonValueKind::kNumber, nullptr,
     8},
    {kResultCount, 2, JsonDelivery::kValue, JsonValueKind::kNumber, nullptr,
     9}};
const JsonField kEspnFields[] = {
    {kLeagues, 1, JsonDelivery::kArrayCount, JsonValueKind::kNumber, nullptr,
     0},
    {kLeague, 2, JsonDelivery::kValue, JsonValueKind::kString, nullptr, 1, 1},
    {kYear, 3, JsonDelivery::kValue, JsonValueKind::kNumber, nullptr, 2, 1},
    {kType, 2, JsonDelivery::kValue, JsonValueKind::kString, nullptr, 3, 1},
    {kWhitelist, 2, JsonDelivery::kValue, JsonValueKind::kBoolean, nullptr, 4,
     1},
    {kStart, 2, JsonDelivery::kValue, JsonValueKind::kString, nullptr, 5, 1},
    {kEnd, 2, JsonDelivery::kValue, JsonValueKind::kString, nullptr, 6, 1},
    {kCalendar, 2, JsonDelivery::kArrayItems, JsonValueKind::kString, nullptr,
     7, 1}};

const char* text(JsonVariantConst value) {
  return value.is<const char*>() ? value.as<const char*>() : "";
}
uint32_t stamp(const char* value) {
  const int64_t at = parseUtc(value);
  return at > 0 && at <= UINT32_MAX ? static_cast<uint32_t>(at) : 0;
}
bool decimal(std::string_view value, uint32_t& result) {
  if (value.empty()) return false;
  uint32_t n = 0;
  for (const char c : value) {
    if (c < '0' || c > '9' || n > (UINT32_MAX - (c - '0')) / 10) return false;
    n = n * 10 + c - '0';
  }
  result = n;
  return true;
}
bool integer(JsonVariantConst value, uint32_t& out) {
  if (value.is<bool>() || !value.is<uint32_t>()) return false;
  out = value.as<uint32_t>();
  return true;
}
bool identityValue(JsonVariantConst value, std::string_view expected) {
  if (value.is<const char*>()) return expected == text(value);
  uint32_t actual = 0, wanted = 0;
  return integer(value, actual) && decimal(expected, wanted) &&
         actual == wanted;
}
bool sameLeague(std::string_view actual, std::string_view expected) {
  if (actual.size() != expected.size()) return false;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    const auto lower = [](char c) {
      return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
    };
    if (lower(actual[i]) != lower(expected[i])) return false;
  }
  return true;
}
bool date(uint32_t at, char (&out)[11]) {
  const time_t stamp = at;
  std::tm tm{};
  return gmtime_r(&stamp, &tm) != nullptr &&
         strftime(out, sizeof(out), "%Y-%m-%d", &tm) == 10;
}
bool dateEquals(JsonVariantConst value, uint32_t at) {
  char expected[11]{};
  return date(at, expected) && std::string_view(text(value)) == expected;
}
bool objectEmpty(JsonVariantConst value) {
  return (value.is<JsonObjectConst>() &&
          value.as<JsonObjectConst>().size() == 0) ||
         (value.is<JsonArrayConst>() && value.as<JsonArrayConst>().size() == 0);
}
bool definite(JsonVariantConst row, cfg::Provider provider) {
  // Providers may publish a provisional UTC timestamp for TBD/SCHEDULED.
  // A time is proof only when its status explicitly treats it as fixed.
  if (provider == cfg::Provider::kApiFootball) {
    const char* status = text(row["fixture"]["status"]["short"]);
    return statusPhase(status) != Phase::kUnknown &&
           std::strcmp(status, "TBD") != 0 && std::strcmp(status, "PST") != 0;
  }
  if (provider == cfg::Provider::kFootballData) {
    const char* status = text(row["status"]);
    return statusPhase(status) != Phase::kUnknown &&
           std::strcmp(status, "SCHEDULED") != 0 &&
           std::strcmp(status, "POSTPONED") != 0;
  }
  if (provider == cfg::Provider::kEspn)
    return row["competitions"][0]["timeValid"].is<bool>() &&
           row["competitions"][0]["timeValid"].as<bool>();
  return true;
}
struct Row {
  uint32_t kickoff = 0;
  bool definite = false, identity = false;
};
Row fixture(JsonVariantConst row, const settings::Source& source,
            std::string_view season) {
  JsonVariantConst timestamp;
  bool same = true;
  if (source.provider == cfg::Provider::kOpenLigaDb) {
    same = sameLeague(text(row["leagueShortcut"]), source.competition.view()) &&
           (season.empty() || identityValue(row["leagueSeason"], season));
    timestamp = row["matchDateTimeUTC"];
  } else if (source.provider == cfg::Provider::kApiFootball) {
    same = identityValue(row["league"]["id"], source.competition.view()) &&
           (season.empty() || identityValue(row["league"]["season"], season));
    timestamp = row["fixture"]["date"];
  } else if (source.provider == cfg::Provider::kFootballData) {
    same = identityValue(row["competition"]["id"], source.competition.view()) ||
           identityValue(row["competition"]["code"], source.competition.view());
    const std::string_view start = text(row["season"]["startDate"]);
    same = same && (season.empty() ||
                    (start.size() >= 4 && start.substr(0, 4) == season));
    timestamp = row["utcDate"];
  } else {
    same = season.empty() || identityValue(row["season"]["year"], season);
    timestamp = row["date"];
  }
  return {stamp(text(timestamp)), definite(row, source.provider), same};
}
}  // namespace

bool observeNormalRow(Evidence& evidence, const settings::Competition& entry,
                      JsonVariantConst row) noexcept {
  const auto mapped = fixture(row, entry.primary, entry.primary.season.view());
  if (!mapped.identity) {
    invalidate(evidence, Reason::kIdentity);
    return true;
  }
  return observeNormal(evidence, identity(entry), mapped.kickoff,
                       mapped.definite);
}

bool discoveryQuery(const settings::Competition& entry,
                    std::string_view resolved_season, uint32_t anchor_utc,
                    uint16_t horizon_days, uint8_t season_index, Query& out,
                    Reason* why) noexcept {
  const auto fail = [why](Reason reason) {
    if (why) *why = reason;
    return false;
  };
  if (!entry.active || entry.id == 0 || entry.incarnation == 0 ||
      entry.primary.competition.empty())
    return fail(Reason::kIdentity);
  if (anchor_utc == 0) return fail(Reason::kClock);
  if (season_index >= cfg::kCoverageMaxSeasons)
    return fail(Reason::kSeasonLimit);
  const auto& source = entry.primary;
  if (static_cast<unsigned>(source.provider) >= cfg::kProviderCount)
    return fail(Reason::kIdentity);
  if (source.provider != cfg::Provider::kOpenLigaDb &&
      source.provider != cfg::Provider::kEspn) {
    if (horizon_days == 0 || horizon_days > 367) return fail(Reason::kRange);
    const uint64_t horizon = uint64_t{horizon_days} * kDay;
    if (anchor_utc <= horizon ||
        uint64_t{anchor_utc} + horizon + kDay > UINT32_MAX)
      return fail(Reason::kRange);
  }
  std::string_view season =
      source.season.empty() ? resolved_season : source.season.view();
  if (season.empty() || !settings::isValidSeason(source.provider, season))
    return fail(Reason::kMissingMetadata);
  char next_season[5]{};
  if (season_index != 0) {
    uint32_t year = 0;
    if (season.size() != 4 || !decimal(season, year) || year >= 9999)
      return fail(Reason::kSeasonLimit);
    std::snprintf(next_season, sizeof(next_season), "%04u", unsigned(year + 1));
    season = next_season;
  }
  // Start constructing only after validation; a rejected query never replaces
  // the caller's current request or points at a temporary string.
  out = {};
  if (!out.season.assign(season)) return fail(Reason::kRange);
  char league[3 * cfg::kCompetitionIdMaxBytes + 1];
  char encoded_season[3 * cfg::kSeasonMaxBytes + 1];
  plan::encode(source.competition.view(), league, sizeof(league));
  plan::encode(season, encoded_season, sizeof(encoded_season));
  switch (source.provider) {
    case cfg::Provider::kOpenLigaDb:
      std::snprintf(out.http.url, sizeof(out.http.url),
                    "https://api.openligadb.de/getmatchdata/%s/%s", league,
                    encoded_season);
      out.http.array = "";
      out.kind = QueryKind::kFullSeason;
      break;
    case cfg::Provider::kEspn:
      std::snprintf(out.http.url, sizeof(out.http.url),
                    "https://site.api.espn.com/apis/site/v2/sports/soccer/%s/"
                    "scoreboard?season=%s",
                    league, encoded_season);
      out.http.array = "events";
      out.kind = QueryKind::kEspnCalendar;
      break;
    default: {
      const uint64_t horizon = uint64_t{horizon_days} * kDay;
      out.kind = QueryKind::kDateRange;
      out.from = (anchor_utc - horizon) / kDay * kDay;
      out.to = (anchor_utc + horizon) / kDay * kDay + kDay - 1;
      char from[11], to[11];
      date(out.from, from);
      // football-data.org excludes dateTo; API-Football includes it.
      date(
          source.provider == cfg::Provider::kFootballData ? out.to + 1 : out.to,
          to);
      if (source.provider == cfg::Provider::kApiFootball) {
        std::snprintf(out.http.url, sizeof(out.http.url),
                      "https://v3.football.api-sports.io/fixtures?league=%s&"
                      "season=%s&from=%s&to=%s",
                      league, encoded_season, from, to);
        out.http.array = "response";
      } else if (source.provider == cfg::Provider::kFootballData) {
        std::snprintf(out.http.url, sizeof(out.http.url),
                      "https://api.football-data.org/v4/competitions/%s/"
                      "matches?dateFrom=%s&dateTo=%s&season=%s",
                      league, from, to, encoded_season);
        out.http.array = "matches";
      }
      break;
    }
  }
  if (why) *why = Reason::kNone;
  return true;
}

void Response::begin(const settings::Competition& entry, const Query& query,
                     uint32_t anchor_utc) noexcept {
  entry_ = &entry;
  query_ = &query;
  anchor_ = anchor_utc;
  first_ = last_ = expected_ = 0;
  calendar_first_ = calendar_last_ = 0;
  metadata_ok_ = 0;
  filter_keys_ = 0;
  bad_metadata_ = source_matches_ = false;
  scan_ = {};
  calendar_ = {};
  observer_ = {};
  const Proof proof = query.kind == QueryKind::kFullSeason ? Proof::kFullSeason
                      : query.kind == QueryKind::kDateRange
                          ? Proof::kDateRange
                          : Proof::kDayWhitelist;
  collector_.begin(identity(entry), query.season.view(), query.from, query.to,
                   anchor_utc, proof);
  switch (entry.primary.provider) {
    case cfg::Provider::kApiFootball:
      observer_ = {kApiFields, 10, metadata, this};
      break;
    case cfg::Provider::kFootballData:
      observer_ = {kFootballDataFields, 10, metadata, this};
      break;
    case cfg::Provider::kEspn:
      observer_ = {kEspnFields, 8, metadata, this};
      break;
    default: break;
  }
}

const JsonObserver* Response::observer() noexcept {
  return observer_.count == 0 ? nullptr : &observer_;
}

void Response::observe(JsonVariantConst row) noexcept {
  if (entry_ == nullptr || query_ == nullptr) return;
  const auto mapped = fixture(row, entry_->primary, query_->season.view());
  if (!mapped.identity) collector_.failed(Reason::kIdentity);
  const uint32_t kickoff = mapped.kickoff;
  collector_.observe(kickoff, mapped.definite);
  if (kickoff != 0) {
    if (first_ == 0 || kickoff < first_) first_ = kickoff;
    last_ = std::max(last_, kickoff);
  }
}

bool Response::metadata(uint8_t tag, JsonVariantConst value, void* context) {
  static_cast<Response*>(context)->field(tag, value);
  return true;
}

void Response::field(uint8_t tag, JsonVariantConst value) noexcept {
  if (entry_ == nullptr || query_ == nullptr || tag >= 16) return;
  const auto& source = entry_->primary;
  bool ok = false;
  uint32_t number = 0;
  if (source.provider == cfg::Provider::kApiFootball) {
    switch (tag) {
      // Raw key counts reject unknown/narrowing and duplicate filters.
      case 0: ok = integer(value, number) && number == 4; break;
      case 1: ok = identityValue(value, source.competition.view()); break;
      case 2: ok = identityValue(value, query_->season.view()); break;
      case 3: ok = dateEquals(value, query_->from); break;
      case 4: ok = dateEquals(value, query_->to); break;
      case 5: ok = integer(value, number) && number == 2; break;
      case 6:
      case 7:
        ok = integer(value, number) && number == 1;
        if (!ok) collector_.failed(Reason::kProviderLimit);
        break;
      case 8: ok = integer(value, expected_); break;
      case 9:
        ok = objectEmpty(value);
        if (!ok) collector_.failed(Reason::kProviderLimit);
        break;
      default: break;
    }
  } else if (source.provider == cfg::Provider::kFootballData) {
    switch (tag) {
      case 0: ok = integer(value, filter_keys_); break;
      case 1: ok = dateEquals(value, query_->from); break;
      case 2: ok = dateEquals(value, query_->to + 1); break;
      case 3: ok = identityValue(value, query_->season.view()); break;
      case 4: ok = true; break;  // Access label, not a narrowing filter.
      case 5:
      case 8: ok = integer(value, number) && number > 0; break;
      case 6:
      case 7:
        ok = value.is<const char*>() || integer(value, number);
        source_matches_ |= identityValue(value, source.competition.view());
        break;
      case 9: ok = integer(value, expected_); break;
      default: break;
    }
  } else if (source.provider == cfg::Provider::kEspn) {
    switch (tag) {
      case 0: ok = integer(value, number) && number == 1; break;
      case 1: ok = sameLeague(text(value), source.competition.view()); break;
      case 2: ok = identityValue(value, query_->season.view()); break;
      case 3: ok = std::strcmp(text(value), "day") == 0; break;
      case 4: ok = value.is<bool>() && value.as<bool>(); break;
      case 5:
        calendar_.from = stamp(text(value));
        ok = calendar_.from != 0;
        break;
      case 6:
        calendar_.to = stamp(text(value));
        ok = calendar_.to != 0;
        break;
      case 7:
        number = stamp(text(value));
        ok = number != 0 && calendar_.count != UINT32_MAX;
        if (ok) {
          ++calendar_.count;
          if (calendar_first_ == 0 || number < calendar_first_)
            calendar_first_ = number;
          calendar_last_ = std::max(calendar_last_, number);
          if (number <= anchor_)
            calendar_.previous_day = std::max(calendar_.previous_day, number);
          else if (calendar_.next_day == 0 || number < calendar_.next_day)
            calendar_.next_day = number;
        }
        break;
      default: break;
    }
  }
  if (ok)
    metadata_ok_ |= uint16_t{1} << tag;
  else
    bad_metadata_ = true;
}

const Span& Response::finish(bool success, uint32_t checked_utc) noexcept {
  if (entry_ == nullptr || query_ == nullptr) {
    collector_.failed(Reason::kIdentity);
    return collector_.finish(checked_utc, {});
  }
  const bool anchored_primary =
      entry_->primary.provider == cfg::Provider::kOpenLigaDb ||
      (scan_.primary_depth == 1 && !scan_.primary_duplicates);
  const bool complete = success && anchored_primary &&
                        scan_.document_complete && !scan_.early_stopped &&
                        !scan_.allocation_failed &&
                        !scan_.invalid_configuration;
  const uint16_t all =
      observer_.count == 0 ? 0 : (uint16_t{1} << observer_.count) - 1;
  const bool football_data =
      entry_->primary.provider == cfg::Provider::kFootballData;
  const uint16_t required =
      football_data ? all & ~((1u << 4) | (1u << 6) | (1u << 7)) : all;
  bool metadata = !bad_metadata_ && (scan_.completed & required) == required &&
                  (scan_.duplicates & all) == 0 &&
                  (scan_.wrong_type & all) == 0 &&
                  (metadata_ok_ & required) == required;
  if (football_data)
    metadata = metadata && source_matches_ &&
               filter_keys_ == 3u + ((scan_.seen & (1u << 4)) != 0);
  bool verified = metadata;
  bool count_known = true;
  if (entry_->primary.provider == cfg::Provider::kOpenLigaDb) {
    collector_.bounds(first_, last_);
    verified = collector_.rows() != 0;
    count_known = false;
  } else if (entry_->primary.provider == cfg::Provider::kEspn) {
    calendar_.verified =
        complete && metadata && calendar_.from <= calendar_.to &&
        calendar_first_ >= calendar_.from && calendar_last_ <= calendar_.to;
    collector_.bounds(calendar_.from, calendar_.to);
    // Calendar timestamps are provider day labels, not kickoff times or
    // UTC day starts. Real Aus/US bodies place matches before/after the label's
    // UTC date. A whitelist alone cannot certify neighbouring kickoff gaps.
    collector_.failed(calendar_.verified ? Reason::kMissingNeighbours
                                         : Reason::kMissingMetadata);
    verified = false;
    count_known = false;
  }
  return collector_.finish(
      checked_utc, {expected_, count_known, complete, verified, metadata});
}

}  // namespace football::coverage
