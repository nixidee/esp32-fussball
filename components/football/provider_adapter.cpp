#include "provider_adapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>

#include "provider_settings.h"

namespace football::provider {
namespace {
template <std::size_t N>
bool encoded(std::string_view input, char (&out)[N]) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::size_t used = 0;
  for (const unsigned char byte : input) {
    const bool plain = (byte >= 'a' && byte <= 'z') ||
                       (byte >= 'A' && byte <= 'Z') ||
                       (byte >= '0' && byte <= '9') || byte == '-' ||
                       byte == '_' || byte == '.' || byte == '~';
    if (byte == 0 || used + (plain ? 1 : 3) >= N) return false;
    if (plain)
      out[used++] = static_cast<char>(byte);
    else {
      out[used++] = '%';
      out[used++] = hex[byte >> 4];
      out[used++] = hex[byte & 15];
    }
  }
  out[used] = 0;
  return true;
}
template <class... Args>
bool url(HttpRequest& out, const char* format, Args... args) {
  const int count = [&] {
    if constexpr (sizeof...(args) == 0)
      return snprintf(out.url, sizeof(out.url), "%s", format);
    else
      return snprintf(out.url, sizeof(out.url), format, args...);
  }();
  return count >= 0 && static_cast<std::size_t>(count) < sizeof(out.url);
}
bool date(uint32_t at, const char* format, char* out, std::size_t capacity) {
  const time_t stamp = at;
  std::tm value{};
  return at && gmtime_r(&stamp, &value) &&
         strftime(out, capacity, format, &value);
}
}  // namespace

Capabilities capabilities(cfg::Provider provider) noexcept {
  switch (provider) {
    case cfg::Provider::kOpenLigaDb: return {true, true, true, true};
    case cfg::Provider::kApiFootball:
    case cfg::Provider::kFootballData: return {true, true, false, true};
    // Calendar days support ordinary fixture retention, but cannot by
    // themselves prove an interval without matches.
    case cfg::Provider::kEspn: return {true, true, true, false};
    default: return {};
  }
}

bool prepareSelection(const SourceInput& source, Operation operation,
                      int64_t utc, SelectionPlan& out) noexcept {
  out = {};
  const bool teams =
      operation == Operation::kTeams || operation == Operation::kOriginalNames;
  if ((!teams && operation != Operation::kCompetitions) ||
      static_cast<unsigned>(source.provider) >= cfg::kProviderCount ||
      !out.season.assign(source.season))
    return false;
  out.discover = teams && source.season.empty() &&
                 (source.provider == cfg::Provider::kOpenLigaDb ||
                  source.provider == cfg::Provider::kApiFootball);
  if (!teams && source.provider == cfg::Provider::kOpenLigaDb &&
      source.season.empty()) {
    if (utc <= 0) return false;
    const time_t at = utc;
    std::tm date{};
    if (!gmtime_r(&at, &date)) return false;
    char year[12];
    snprintf(year, sizeof(year), "%d",
             date.tm_year + 1900 -
                 (date.tm_mon + 1 < cfg::kSeasonStartMonth ? 1 : 0));
    return out.season.assign(year);
  }
  return true;
}

bool buildRequest(const RequestSpec& request, HttpRequest& out) noexcept {
  out = {};
  const auto& source = request.source;
  const auto operation = request.operation;
  if (source.competition.size() > cfg::kCompetitionIdMaxBytes ||
      source.season.size() > cfg::kSeasonMaxBytes ||
      request.country.size() > cfg::kSelectionTextMaxBytes ||
      (operation != Operation::kCompetitions && source.competition.empty()))
    return false;
  char league[3 * cfg::kCompetitionIdMaxBytes + 1];
  char season[3 * cfg::kSeasonMaxBytes + 1];
  char country[3 * cfg::kSelectionTextMaxBytes + 1];
  if (!encoded(source.competition, league) || !encoded(source.season, season) ||
      !encoded(request.country.empty() ? cfg::kSelectionCountryDefault
                                       : request.country,
               country))
    return false;
  if (operation == Operation::kTeams ||
      operation == Operation::kOriginalNames ||
      operation == Operation::kCompetitions)
    out.body_limit = cfg::kSelectionBodyBytes;
  const bool teams =
      operation == Operation::kTeams || operation == Operation::kOriginalNames;
  if (source.provider == cfg::Provider::kOpenLigaDb) {
    constexpr const char* base = "https://api.openligadb.de/";
    switch (operation) {
      case Operation::kFixtures:
        if (!request.round && !request.team && !source.season.empty())
          out.body_limit = cfg::kProviderSeasonBodyBytes;
        if (request.team) {
          const unsigned weeks = (cfg::kFixtureHorizonDays + 6) / 7;
          return url(out, "%sgetmatchesbyteamid/%lu/%u/%u", base,
                     static_cast<unsigned long>(request.team), weeks, weeks);
        }
        if (request.round)
          return !source.season.empty() &&
                 url(out, "%sgetmatchdata/%s/%s/%u", base, league, season,
                     static_cast<unsigned>(request.round));
        if (!source.season.empty())
          return url(out, "%sgetmatchdata/%s/%s", base, league, season);
        return url(out, "%sgetmatchdata/%s", base, league);
      case Operation::kCurrentSeason:
      case Operation::kCurrentRound:
        return url(out, "%sgetmatchdata/%s", base, league);
      case Operation::kLastChange:
        return request.round && !source.season.empty() &&
               url(out, "%sgetlastchangedate/%s/%s/%u", base, league, season,
                   static_cast<unsigned>(request.round));
      case Operation::kTable:
        return !source.season.empty() &&
               url(out, "%sgetbltable/%s/%s", base, league, season);
      case Operation::kTeams:
      case Operation::kOriginalNames:
        return !source.season.empty() &&
               url(out, "%sgetavailableteams/%s/%s", base, league, season);
      case Operation::kCompetitions:
        return !source.season.empty() &&
               url(out, "%sgetavailableleagues/%s", base, season);
      default: return false;
    }
  }
  if (source.provider == cfg::Provider::kEspn) {
    constexpr const char* site =
        "https://site.api.espn.com/apis/site/v2/sports/soccer/";
    if (operation == Operation::kCompetitions)
      return url(out,
                 "https://site.api.espn.com/apis/site/v2/leagues/"
                 "dropdown?sport=soccer&limit=1000");
    if (teams)
      return url(out, "%s%s/teams?limit=1000%s%s", site, league,
                 source.season.empty() ? "" : "&season=", season);
    switch (operation) {
      case Operation::kOwnFixtures:
        out.body_limit = cfg::kProviderSeasonBodyBytes;
        return request.team &&
               url(out, "%s%s/teams/%lu/schedule%s%s", site, league,
                   static_cast<unsigned long>(request.team),
                   source.season.empty() ? "" : "?season=", season);
      case Operation::kTable:
        return url(out,
                   "https://site.web.api.espn.com/apis/v2/sports/soccer/%s/"
                   "standings%s%s",
                   league, source.season.empty() ? "" : "?season=", season);
      case Operation::kCurrentSeason:
      case Operation::kCurrentRound:
        return url(out, "%s%s/scoreboard", site, league);
      case Operation::kCalendarDays:
      case Operation::kSeasonCalendar:
        if (!source.season.empty())
          return url(
              out,
              "https://sports.core.api.espn.com/v2/sports/soccer/leagues/"
              "%s/seasons/%s/types/1/calendar/ondays",
              league, season);
        return url(out,
                   "https://sports.core.api.espn.com/v2/sports/soccer/leagues/"
                   "%s/calendar/ondays",
                   league);
      case Operation::kSummary:
        out.body_limit = cfg::kProviderSummaryBodyBytes;
        return request.match && url(out, "%s%s/summary?event=%lu", site, league,
                                    static_cast<unsigned long>(request.match));
      case Operation::kFixtures: {
        char from[9]{}, to[9]{};
        if (request.to && request.to < request.from) return false;
        if (!request.from)
          return url(out, "%s%s/scoreboard%s%s", site, league,
                     source.season.empty() ? "" : "?season=", season);
        if (!date(request.from, "%Y%m%d", from, sizeof(from))) return false;
        if (request.to && request.to >= request.from) {
          if (!date(request.to, "%Y%m%d", to, sizeof(to))) return false;
          if (!strcmp(from, to)) to[0] = 0;
        }
        return url(out, "%s%s/scoreboard?dates=%s%s%s%s%s", site, league, from,
                   to[0] ? "-" : "", to,
                   source.season.empty() ? "" : "&season=", season);
      }
      default: return false;
    }
  }
  if (source.provider == cfg::Provider::kApiFootball) {
    constexpr const char* base = "https://v3.football.api-sports.io/";
    if (operation == Operation::kCompetitions)
      return url(out, "%sleagues?country=%s", base, country);
    if (operation == Operation::kCurrentSeason)
      return url(out, "%sleagues?id=%s&current=true", base, league);
    if (source.season.empty()) return false;
    if (teams)
      return url(out, "%steams?league=%s&season=%s", base, league, season);
    if (operation == Operation::kTable)
      return url(out, "%sstandings?league=%s&season=%s", base, league, season);
    if (operation == Operation::kFixtures && !request.from && !request.to) {
      out.body_limit = cfg::kProviderSeasonBodyBytes;
      return url(out, "%sfixtures?league=%s&season=%s", base, league, season);
    }
    char from[11], to[11];
    return operation == Operation::kFixtures && request.to >= request.from &&
           date(request.from, "%Y-%m-%d", from, sizeof(from)) &&
           date(request.to, "%Y-%m-%d", to, sizeof(to)) &&
           url(out, "%sfixtures?league=%s&season=%s&from=%s&to=%s", base,
               league, season, from, to);
  }
  if (source.provider == cfg::Provider::kFootballData) {
    constexpr const char* base =
        "https://api.football-data.org/v4/competitions";
    if (operation == Operation::kCompetitions) return url(out, "%s", base);
    if (teams || operation == Operation::kTable)
      return url(out, "%s/%s/%s%s%s", base, league,
                 teams ? "teams" : "standings",
                 source.season.empty() ? "" : "?season=", season);
    if (operation == Operation::kFixtures && !request.from && !request.to) {
      out.body_limit = cfg::kProviderSeasonBodyBytes;
      return url(out, "%s/%s/matches%s%s", base, league,
                 source.season.empty() ? "" : "?season=", season);
    }
    char from[11], to[11];
    return operation == Operation::kFixtures && request.to >= request.from &&
           request.to <= UINT32_MAX - 86400 &&
           date(request.from, "%Y-%m-%d", from, sizeof(from)) &&
           date(request.to + 86400, "%Y-%m-%d", to, sizeof(to)) &&
           url(out, "%s/%s/matches?dateFrom=%s&dateTo=%s%s%s", base, league,
               from, to, source.season.empty() ? "" : "&season=", season);
  }
  return false;
}
Authentication authentication(
    cfg::Provider provider, const settings::ProviderSettings& model) noexcept {
  switch (provider) {
    case cfg::Provider::kApiFootball:
      return {"x-apisports-key", model.api_football_key.view()};
    case cfg::Provider::kFootballData:
      return {"X-Auth-Token", model.football_data_key.view()};
    default: return {};
  }
}
}  // namespace football::provider
