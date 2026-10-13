#include "football_service.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_service.h"
#include "ota_service.h"
#include "provider_client.h"
#include "request_budget.h"
#include "settings_store.h"
#include "time_service.h"
namespace football {
namespace {
Snapshot slots[2];
Snapshot route_scratch;
uint8_t published = 0;
StaticSemaphore_t mutex_buffer;
SemaphoreHandle_t mutex = nullptr;
StaticTask_t task_buffer;
StackType_t stack[cfg::kProviderStackBytes / sizeof(StackType_t)];
std::atomic<bool> requested{true}, selecting{false}, busy{false};
std::atomic<bool> time_changed{false};
SelectionRequest selection_request;
Selection selections[cfg::kSelectionEntries];
SelectionStatus selection_state{};
// Bit i set: competition i failed in the last round and shows earlier data
// (mutex-protected, like selection_state).
uint8_t error_routes = 0;
// A failed competition is fetched again at 'retry_at' (monotonic ms); until
// then its earlier data is carried over. 'reason' and 'cause' explain the
// failure (static texts). Provider task only.
struct RouteFailure {
  int64_t retry_at = 0;
  const char* reason = nullptr;
  Cause cause = Cause::kNone;
};
RouteFailure route_failures[cfg::kCompetitionMax];
uint32_t requests[cfg::kProviderCount]{}, day_requests[cfg::kProviderCount]{};
int64_t last_request[cfg::kProviderCount]{}, day = 0, due = 0;
uint32_t last_generation = 0, last_epoch = 0;
// Resolved current season of an empty season setting, per source (0 primary,
// 1 fallback); valid for kPollIdleS.
struct SeasonCache {
  char text[cfg::kSeasonMaxBytes + 1]{};
  int64_t at = 0;
};
struct RouteCache {
  SeasonCache seasons[2];
  char change[64]{};
  int round = 0;
  int64_t full_at = 0;
};
RouteCache caches[cfg::kCompetitionMax];
bool baseline = true;
// timekeeping::Status::step_count seen by the worker.
uint32_t time_steps = 0;
const char* str(ArduinoJson::JsonVariantConst v) {
  return v.is<const char*>() ? v.as<const char*>() : "";
}
// One URL path or query component: every byte outside the RFC 3986
// unreserved set is percent-encoded (OpenLigaDB shortcuts may contain spaces
// or umlauts). 'out' holds three bytes per input byte plus the terminator.
template <std::size_t N>
void encode(std::string_view in, char (&out)[N]) {
  static_assert(N >= 4);
  std::size_t at = 0;
  for (const char ch : in) {
    const auto c = static_cast<unsigned char>(ch);
    if (at + 4 > N) break;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~')
      out[at++] = static_cast<char>(c);
    else
      at += snprintf(out + at, 4, "%%%02X", c);
  }
  out[at] = '\0';
}
void identity(Id& out, ArduinoJson::JsonVariantConst v) {
  if (v.is<const char*>())
    out.assign(str(v));
  else if (v.is<uint32_t>()) {
    char b[16];
    snprintf(b, sizeof(b), "%lu", static_cast<unsigned long>(v.as<uint32_t>()));
    out.assign(b);
  }
}
void lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
void unlock() { xSemaphoreGive(mutex); }
// Why the last get() failed. Provider task only; the code pairs the text and
// the cause with the error they explain.
const char* failure = nullptr;
Cause failure_cause = Cause::kNone;
esp_err_t failure_code = ESP_OK;
void clearFailure() {
  failure = nullptr;
  failure_cause = Cause::kNone;
  failure_code = ESP_OK;
}
esp_err_t stop(const char* why, esp_err_t err, Cause cause) {
  failure = why;
  failure_cause = cause;
  failure_code = err;
  return err;
}
// Distinct text for the logs and the Web UI; plain error name otherwise.
const char* reason(esp_err_t err) {
  return failure != nullptr && failure_code == err ? failure
                                                   : esp_err_to_name(err);
}
// Stable key behind reason(): the cause recorded for this error, else one
// derived from the plain error code (failures outside get()).
Cause causeOf(esp_err_t err) {
  if (failure_cause != Cause::kNone && failure_code == err)
    return failure_cause;
  switch (err) {
    case ESP_ERR_NO_MEM: return Cause::kMemory;
    case ESP_ERR_INVALID_SIZE: return Cause::kTooLarge;
    case ESP_ERR_INVALID_RESPONSE: return Cause::kBadResponse;
    default: return Cause::kOther;
  }
}
// A request that was sent: the error of fetchJson() and the HTTP status it
// received (0 = none, so no response came).
Cause fetchCause(esp_err_t err, int status) {
  if (err == ESP_ERR_NO_MEM) return Cause::kMemory;
  if (err == ESP_ERR_INVALID_SIZE) return Cause::kTooLarge;
  if (err == ESP_ERR_INVALID_RESPONSE) return Cause::kBadResponse;
  if (status == 429) return Cause::kRateLimited;
  if (status == 401 || status == 403) return Cause::kAuth;
  if (status == 404) return Cause::kNotFound;
  if (status == 0) return Cause::kConnect;
  return status != 200 ? Cause::kHttp : Cause::kOther;
}
struct Interruption {
  const char* why = nullptr;
  Cause cause = Cause::kNone;
  explicit operator bool() const { return why != nullptr; }
};
Interruption interrupted(uint32_t generation, uint32_t epoch) {
  if (settings::dataGeneration() != generation)
    return {"settings changed", Cause::kSettings};
  if (!network::ready()) return {"network lost", Cause::kNetwork};
  if (network::status().epoch != epoch)
    return {"network changed", Cause::kNetwork};
  return {};
}
esp_err_t get(const char* url, const char* array, cfg::Provider p,
              const settings::Model& m, uint32_t generation,
              JsonConsumer consumer, void* context, bool selection = false) {
  clearFailure();
  // One log line per provider request, sent or not. Keys travel in headers,
  // never in the URL.
  const auto skip = [url](const char* why, esp_err_t err, Cause cause) {
    ESP_LOGW("provider", "not sent: %s (%s)", url, why);
    return stop(why, err, cause);
  };
  if (!settings::providerUsable(p, m))
    return skip("provider not enabled", ESP_ERR_INVALID_STATE,
                Cause::kProviderOff);
  const auto index = static_cast<unsigned>(p);
  const auto now = timekeeping::monotonicMs();
  const unsigned daily_limit = p == cfg::Provider::kApiFootball
                                   ? m.api_daily_budget
                                   : cfg::kUnbudgetedDailyRequestCap;
  const int64_t remaining =
      cfg::kProviderMinIntervalMs[index] - (now - last_request[index]);
  const uint32_t epoch = network::status().epoch;
  const int64_t ready_at =
      now + std::max<int64_t>(0, last_request[index] > 0 ? remaining : 0);
  while (timekeeping::monotonicMs() < ready_at) {
    if (const auto stopped = interrupted(generation, epoch))
      return skip(stopped.why, ESP_ERR_INVALID_STATE, stopped.cause);
    vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
  }
  if (const auto stopped = interrupted(generation, epoch))
    return skip(stopped.why, ESP_ERR_INVALID_STATE, stopped.cause);
  const int64_t deadline =
      timekeeping::monotonicMs() + cfg::kOperationDeadlineMs;
  while (!network::tryHeavy()) {
    if (const auto stopped = interrupted(generation, epoch))
      return skip(stopped.why, ESP_ERR_INVALID_STATE, stopped.cause);
    if (timekeeping::monotonicMs() >= deadline)
      return skip("operation busy", ESP_ERR_TIMEOUT, Cause::kBusy);
    vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
  }
  const auto admitted =
      budget::admit(p, static_cast<uint32_t>(day), daily_limit);
  if (admitted != ESP_OK) {
    network::releaseHeavy();
    if (admitted == ESP_ERR_NOT_ALLOWED)
      return skip("daily budget used", admitted, Cause::kBudget);
    return skip("budget unavailable", admitted, Cause::kOther);
  }
  last_request[index] = timekeeping::monotonicMs();
  lock();
  ++requests[index];
  ++day_requests[index];
  unlock();
  FetchResult result;
  const int64_t started = timekeeping::monotonicMs();
  const auto err = fetchJson(url, array, p, m, generation, consumer, context,
                             selection, &result);
  network::releaseHeavy();
  const auto elapsed =
      static_cast<long long>(timekeeping::monotonicMs() - started);
  if (err == ESP_OK) {
    ESP_LOGI("provider", "%s: HTTP %d, %u B, %lld ms", url, result.status,
             static_cast<unsigned>(result.bytes), elapsed);
  } else {
    ESP_LOGW("provider", "%s: HTTP %d, %u B, %lld ms, %s", url, result.status,
             static_cast<unsigned>(result.bytes), elapsed,
             esp_err_to_name(err));
    if (const auto stopped = interrupted(generation, epoch))
      stop(stopped.why, err, stopped.cause);
    else {
      failure_cause = fetchCause(err, result.status);
      failure_code = err;
    }
  }
  return err;
}
template <class T>
void tag(Value<T>& value, cfg::Provider provider) {
  if (value.source == Source::kProvider)
    value.source = providerSource(provider);
}
void tagScore(Score& value, cfg::Provider provider) {
  tag(value.home, provider);
  tag(value.away, provider);
}
void tagMatch(Snapshot& data, Match& m, const settings::Source& route) {
  m.provider = m.home.provider = m.away.provider = route.provider;
  m.own = route.team.view() == m.home.id.data ||
          route.team.view() == m.away.id.data;
  tag(m.round, route.provider);
  tag(m.kickoff, route.provider);
  tag(m.finished_at, route.provider);
  tag(m.provider_changed, route.provider);
  tag(m.minute, route.provider);
  if (m.phase_source == Source::kProvider)
    m.phase_source = providerSource(route.provider);
  for (auto* s :
       {&m.principal, &m.halftime, &m.regulation, &m.extra_time, &m.shootout})
    tagScore(*s, route.provider);
  for (unsigned i = 0; i < m.event_count; ++i) {
    auto& event = data.events[m.events[i]];
    tag(event.minute, event.provider);
    tag(event.added, event.provider);
    tagScore(event.after, event.provider);
  }
}
bool appendRoute(Snapshot& target, Snapshot& from,
                 const settings::Source& route) {
  for (unsigned i = 0; i < from.match_count; ++i) {
    tagMatch(from, from.matches[i], route);
    if (!appendMatch(target, from, from.matches[i])) return false;
  }
  if (from.table_count) {
    target.table_count = from.table_count;
    for (unsigned i = 0; i < from.table_count; ++i) {
      auto row = from.table[i];
      row.team.provider = route.provider;
      row.own = route.team.view() == row.team.id.data;
      for (auto* v :
           {&row.position, &row.played, &row.won, &row.drawn, &row.lost,
            &row.goals_for, &row.goals_against, &row.difference, &row.points})
        tag(*v, route.provider);
      target.table[i] = row;
    }
  }
  return true;
}
struct Mapping {
  Snapshot* out;
  cfg::Provider provider;
  uint8_t route;
  int64_t now;
  bool table;
  FixtureFilter filter;
};
bool consume(ArduinoJson::JsonVariantConst row, void* context) {
  auto& c = *static_cast<Mapping*>(context);
  return c.table ? mapTable(row, *c.out, c.provider)
                 : mapMatch(row, *c.out, c.provider, c.route, c.now, c.filter);
}
// The club's fixtures across all competitions; only rows of this route's
// competition and season are mapped.
struct LeagueRows {
  Mapping* mapping;
  const char* competition;
  int season;
};
bool consumeLeague(ArduinoJson::JsonVariantConst row, void* context) {
  auto& c = *static_cast<LeagueRows*>(context);
  if (strcasecmp(str(row["leagueShortcut"]), c.competition) != 0 ||
      row["leagueSeason"].as<int>() != c.season)
    return true;
  return consume(row, c.mapping);
}
bool changed(ArduinoJson::JsonVariantConst row, void* context) {
  auto* buffer = static_cast<char*>(context);
  if (!row.is<const char*>() || strlen(str(row)) >= 64) return false;
  snprintf(buffer, 64, "%s", str(row));
  return true;
}
bool group(ArduinoJson::JsonVariantConst row, void* context) {
  auto* result = static_cast<int*>(context);
  if (!row["groupOrderID"].is<int>()) return false;
  *result = row["groupOrderID"].as<int>();
  return *result > 0 && *result <= 1000;
}
bool season(ArduinoJson::JsonVariantConst row, void* context) {
  auto* route = static_cast<settings::Source*>(context);
  if (row["leagueSeason"].is<int>()) {
    char b[cfg::kSeasonMaxBytes + 1];
    snprintf(b, sizeof(b), "%d", row["leagueSeason"].as<int>());
    return route->season.assign(b);
  }
  return false;
}
// API-Football league row: the season marked current.
bool apiSeason(ArduinoJson::JsonVariantConst row, void* context) {
  auto* route = static_cast<settings::Source*>(context);
  for (auto item : row["seasons"].as<ArduinoJson::JsonArrayConst>())
    if (item["current"].as<bool>() && item["year"].is<int>()) {
      char b[cfg::kSeasonMaxBytes + 1];
      snprintf(b, sizeof(b), "%d", item["year"].as<int>());
      return route->season.assign(b);
    }
  return true;
}
// Fills the empty season of 'route' with the provider's current season (one
// request). Only OpenLigaDB and API-Football need it in their URLs; the
// others select their current season themselves when it is left out.
esp_err_t currentSeason(settings::Source& route, const char* comp_url,
                        const settings::Model& model, uint32_t generation) {
  char url[160];
  esp_err_t err;
  if (route.provider == cfg::Provider::kOpenLigaDb) {
    // The current round's matches carry leagueSeason (about 18 KB for bl1).
    snprintf(url, sizeof(url), "https://api.openligadb.de/getmatchdata/%s",
             comp_url);
    err = get(url, "", route.provider, model, generation, season, &route,
              true);
  } else {
    snprintf(url, sizeof(url),
             "https://v3.football.api-sports.io/leagues?id=%s&current=true",
             comp_url);
    err = get(url, "response", route.provider, model, generation, apiSeason,
              &route, true);
  }
  if (err == ESP_OK && route.season.empty()) err = ESP_ERR_INVALID_RESPONSE;
  return err;
}
void dates(int64_t utc, char* from, char* to, bool compact) {
  time_t start = utc - cfg::kFixtureHorizonDays * 86400LL,
         end = utc + cfg::kFixtureHorizonDays * 86400LL;
  std::tm a{}, b{};
  gmtime_r(&start, &a);
  gmtime_r(&end, &b);
  strftime(from, 16, compact ? "%Y%m%d" : "%Y-%m-%d", &a);
  strftime(to, 16, compact ? "%Y%m%d" : "%Y-%m-%d", &b);
}
// 'table_source' marks the competition that supplies the league table (the
// first active one). 'events' false skips the per-match event requests of
// API-Football (the fixture picker needs only the match list).
esp_err_t routeFetch(settings::Source route, uint8_t index, bool table_source,
                     const settings::Model& model, uint32_t generation,
                     Snapshot& target, int64_t utc, bool primary = true,
                     bool events = true) {
  clearFailure();
  char url[384], competition[cfg::kCompetitionIdMaxBytes + 1]{},
      season_text[cfg::kSeasonMaxBytes + 1]{};
  memcpy(competition, route.competition.data.data(), route.competition.length);
  memcpy(season_text, route.season.data.data(), route.season.length);
  const auto p = route.provider;
  char own_team[cfg::kTeamIdMaxBytes + 1]{};
  memcpy(own_team, route.team.data.data(), route.team.length);
  // URL forms; the raw texts above are compared with provider rows.
  char comp_url[3 * cfg::kCompetitionIdMaxBytes + 1],
      team_url[3 * cfg::kTeamIdMaxBytes + 1];
  encode(competition, comp_url);
  encode(own_team, team_url);
  Mapping mapping{&target, p, index, utc, false, {own_team, 0}};
  esp_err_t err;
  auto& cache = caches[index];
  if (route.season.empty() &&
      (p == cfg::Provider::kOpenLigaDb || p == cfg::Provider::kApiFootball)) {
    auto& known = cache.seasons[primary ? 0 : 1];
    if (known.text[0] &&
        timekeeping::monotonicMs() - known.at < cfg::kPollIdleS * 1000LL) {
      route.season.assign(known.text);
    } else {
      err = currentSeason(route, comp_url, model, generation);
      if (err != ESP_OK) return err;
      memcpy(known.text, route.season.data.data(), route.season.length);
      known.text[route.season.length] = '\0';
      known.at = timekeeping::monotonicMs();
    }
    memcpy(season_text, route.season.data.data(), route.season.length);
  }
  if (p == cfg::Provider::kOpenLigaDb) {
    // Cheap change endpoint suppresses full refreshes of the current round.
    int round = 0;
    snprintf(url, sizeof(url), "https://api.openligadb.de/getcurrentgroup/%s",
             comp_url);
    err = get(url, nullptr, p, model, generation, group, &round);
    if (err != ESP_OK) return err;
    mapping.filter.round = round;
    char timestamp[64]{};
    snprintf(url, sizeof(url),
             "https://api.openligadb.de/getlastchangedate/%s/%s/%d",
             comp_url, season_text, round);
    err = get(url, nullptr, p, model, generation, changed, timestamp);
    if (err != ESP_OK) return err;
    if (primary && !baseline && cache.round == round &&
        strcmp(cache.change, timestamp) == 0 &&
        timekeeping::monotonicMs() - cache.full_at <
            cfg::kCatchupRefreshS * 1000LL) {
      const auto& prior = slots[published];
      for (unsigned i = 0; i < prior.match_count; ++i) {
        if (prior.matches[i].route != index || prior.matches[i].provider != p)
          continue;
        auto match = prior.matches[i];
        // Cached primary values must not retain yesterday's composition.
        // Clear fields whose provenance belongs to the optional provider.
        auto primary = [&](auto& field) {
          if (confirmed(field.source) && field.source != providerSource(p))
            field = {};
        };
        primary(match.minute);
        primary(match.kickoff);
        for (auto* score :
             {&match.principal, &match.halftime, &match.regulation,
              &match.extra_time, &match.shootout}) {
          primary(score->home);
          primary(score->away);
        }
        if (confirmed(match.phase_source) &&
            match.phase_source != providerSource(p)) {
          match.phase_source = Source::kNone;
          match.phase = Phase::kUnknown;
        }
        estimate(match, utc);
        if (!appendMatch(target, prior, match, true))
          return ESP_ERR_INVALID_SIZE;
      }
      if (table_source && prior.table_count &&
          prior.table[0].team.provider == p) {
        target.table = prior.table;
        target.table_count = prior.table_count;
      }
      return ESP_OK;
    }
    // keep() retains the current round and the club's fixtures within
    // kFixtureHorizonDays. The round request and the club's fixture request
    // cover exactly that, including catch-up fixtures outside the round,
    // without streaming the whole season (about 300 KB for bl1).
    snprintf(url, sizeof(url),
             "https://api.openligadb.de/getmatchdata/%s/%s/%d", comp_url,
             season_text, round);
    err = get(url, "", p, model, generation, consume, &mapping);
    if (err == ESP_OK && own_team[0]) {
      const unsigned weeks = (cfg::kFixtureHorizonDays + 6) / 7;
      LeagueRows rows{&mapping, competition, atoi(season_text)};
      snprintf(url, sizeof(url),
               "https://api.openligadb.de/getmatchesbyteamid/%s/%u/%u",
               team_url, weeks, weeks);
      err = get(url, "", p, model, generation, consumeLeague, &rows);
    }
    if (table_source && err == ESP_OK) {
      mapping.table = true;
      snprintf(url, sizeof(url), "https://api.openligadb.de/getbltable/%s/%s",
               comp_url, season_text);
      err = get(url, "", p, model, generation, consume, &mapping);
    }
    if (err == ESP_OK && primary) {
      snprintf(cache.change, sizeof(cache.change), "%s", timestamp);
      cache.round = round;
      cache.full_at = timekeeping::monotonicMs();
    }
  } else if (p == cfg::Provider::kApiFootball) {
    char from[16], to[16];
    dates(utc, from, to, false);
    snprintf(url, sizeof(url),
             "https://v3.football.api-sports.io/"
             "fixtures?league=%s&season=%s&from=%s&to=%s",
             comp_url, season_text, from, to);
    err = get(url, "response", p, model, generation, consume, &mapping);
    if (table_source && err == ESP_OK) {
      mapping.table = true;
      snprintf(
          url, sizeof(url),
          "https://v3.football.api-sports.io/standings?league=%s&season=%s",
          comp_url, season_text);
      err = get(url, "response", p, model, generation, consume, &mapping);
    }
    if (err == ESP_OK && events)
      for (unsigned i = 0; i < target.match_count; ++i) {
        auto& match = target.matches[i];
        if (match.route != index || (route.team.view() != match.home.id.data &&
                                     route.team.view() != match.away.id.data))
          continue;
        struct Events {
          Snapshot* out;
          Match* match;
        } context{&target, &match};
        snprintf(url, sizeof(url),
                 "https://v3.football.api-sports.io/fixtures/events?fixture=%s",
                 match.id.data);
        err = get(
            url, "response", p, model, generation,
            [](ArduinoJson::JsonVariantConst row, void* c) {
              auto& e = *static_cast<Events*>(c);
              return mapEvent(row, *e.out, *e.match,
                              cfg::Provider::kApiFootball);
            },
            &context);
        if (err != ESP_OK) break;
      }
  } else if (p == cfg::Provider::kFootballData) {
    char from[16], to[16];
    dates(utc, from, to, false);
    snprintf(url, sizeof(url),
             "https://api.football-data.org/v4/competitions/%s/"
             "matches?dateFrom=%s&dateTo=%s%s%s",
             comp_url, from, to,
             route.season.empty() ? "" : "&season=", season_text);
    err = get(url, "matches", p, model, generation, consume, &mapping);
    if (table_source && err == ESP_OK) {
      mapping.table = true;
      snprintf(url, sizeof(url),
               "https://api.football-data.org/v4/competitions/%s/standings%s%s",
               comp_url,
               route.season.empty() ? "" : "?season=", season_text);
      err = get(url, "standings", p, model, generation, consume, &mapping);
    }
  } else {
    char today[16];
    time_t stamp = utc;
    std::tm date{};
    gmtime_r(&stamp, &date);
    strftime(today, sizeof(today), "%Y%m%d", &date);
    snprintf(url, sizeof(url),
             "https://site.api.espn.com/apis/site/v2/sports/soccer/%s/"
             "scoreboard?dates=%s%s%s",
             comp_url, today,
             route.season.empty() ? "" : "&season=", season_text);
    err = get(url, "events", p, model, generation, consume, &mapping);
    // Soccer scoreboard range requests returned HTTP 400 during contract
    // research. The team's fixture schedule supplies upcoming own matches.
    if (err == ESP_OK) {
      snprintf(
          url, sizeof(url),
          "https://site.api.espn.com/apis/site/v2/sports/soccer/%s/teams/%s/"
          "schedule?fixture=true%s%s",
          comp_url, team_url,
          route.season.empty() ? "" : "&season=", season_text);
      err = get(url, "events", p, model, generation, consume, &mapping, true);
    }
    if (err == ESP_OK) {
      snprintf(
          url, sizeof(url),
          "https://site.api.espn.com/apis/site/v2/sports/soccer/%s/teams/%s/"
          "schedule%s%s",
          comp_url, team_url,
          route.season.empty() ? "" : "?season=", season_text);
      err = get(url, "events", p, model, generation, consume, &mapping, true);
    }
    if (table_source && err == ESP_OK) {
      mapping.table = true;
      snprintf(url, sizeof(url),
               "https://site.web.api.espn.com/apis/v2/sports/soccer/%s/"
               "standings%s%s",
               comp_url,
               route.season.empty() ? "" : "?season=", season_text);
      err = get(url, "entries", p, model, generation, consume, &mapping);
    }
  }
  return err;
}
struct SelectContext {
  const SelectionRequest* request;
  unsigned seen = 0;
};
// Lower case for the list search: ASCII and the UTF-8 Latin-1 capitals
// (U+00C0..U+00DE except U+00D7, e.g. "Ä", "Ö", "Ü"), so "köln" finds "Köln".
template <std::size_t N>
void fold(char (&text)[N]) {
  for (std::size_t i = 0; i < N && text[i]; ++i) {
    auto& ch = reinterpret_cast<unsigned char&>(text[i]);
    if (ch >= 'A' && ch <= 'Z') {
      ch += 32;
    } else if (ch == 0xc3 && i + 1 < N) {
      auto& next = reinterpret_cast<unsigned char&>(text[i + 1]);
      if (next >= 0x80 && next <= 0x9e && next != 0x97) next += 0x20;
      if (next) ++i;
    }
  }
}
bool selectionRow(ArduinoJson::JsonVariantConst row, void* context) {
  auto& c = *static_cast<SelectContext*>(context);
  const auto& request = *c.request;
  Selection s;
  const auto p = request.provider;
  if (p == cfg::Provider::kOpenLigaDb) {
    // The season list also carries test and other sports' competitions.
    constexpr int kFootball = 1;
    if (!request.teams && row["sport"]["sportId"].as<int>() != kFootball)
      return true;
    identity(s.id, row[request.teams ? "teamId" : "leagueShortcut"]);
    s.name.assign(str(row[request.teams ? "teamName" : "leagueName"]));
    identity(s.season, row["leagueSeason"]);
  } else if (p == cfg::Provider::kApiFootball) {
    auto r = row[request.teams ? "team" : "league"];
    identity(s.id, r["id"]);
    s.name.assign(str(r["name"]));
    if (!request.teams)
      for (auto item : row["seasons"].as<ArduinoJson::JsonArrayConst>())
        if (item["current"].as<bool>()) identity(s.season, item["year"]);
  } else if (p == cfg::Provider::kFootballData) {
    identity(s.id, row["id"]);
    s.name.assign(str(row["name"]));
    if (!request.teams) {
      const char* date = str(row["currentSeason"]["startDate"]);
      if (strlen(date) >= 4) s.season.assign(std::string_view(date, 4));
    }
  } else {
    auto r = request.teams ? row["team"] : row;
    identity(s.id, r[request.teams ? "id" : "slug"]);
    s.name.assign(str(r[request.teams ? "displayName" : "name"]));
  }
  if (s.id.empty()) return true;
  if (!request.search.empty()) {
    char hay[65]{}, needle[49]{};
    snprintf(hay, sizeof(hay), "%s", s.name.data);
    snprintf(needle, sizeof(needle), "%s", request.search.data);
    fold(hay);
    fold(needle);
    if (strstr(hay, needle) == nullptr) return true;
  }
  if (c.seen++ < request.offset) return true;
  lock();
  if (selection_state.count >= cfg::kSelectionEntries)
    selection_state.more = true;
  else
    selections[selection_state.count++] = s;
  unlock();
  return true;
}
void finishSelection(esp_err_t err) {
  lock();
  if (err != ESP_OK) {
    // Distinct codes so the Web UI can explain each cause.
    selection_state.error.assign(reason(err));
    selection_state.cause = causeOf(err);
    selection_state.count = 0;
  } else {
    selection_state.error = {};
    selection_state.cause = Cause::kNone;
  }
  selection_state.busy = false;
  ++selection_state.revision;
  // Cleared under the mutex: select() checks it there before a new job.
  selecting.store(false);
  unlock();
}
// Picker entry of a match: provider match id, kickoff (UTC seconds as text,
// empty if unknown) and "home - away".
void fixtureEntry(const Match& m, Selection& out) {
  out = {};
  out.id.assign(m.id.data);
  if (m.kickoff.valid()) {
    char text[24];
    snprintf(text, sizeof(text), "%lld",
             static_cast<long long>(m.kickoff.value));
    out.season.assign(text);
  }
  char label[2 * cfg::kNameBytes + 4];
  snprintf(label, sizeof(label), "%s - %s", m.home.name.data,
           m.away.name.data);
  out.name.assign(label);
}
// Loads the club's matches at the fallback source of competition 'route' into
// the selection table. Same data a fallback round would use for pairing.
esp_err_t runFixtures(const settings::Model& model, uint32_t generation,
                      uint8_t route, int64_t utc) {
  if (route >= model.competitions.size() ||
      !model.competitions[route].fallback.complete())
    return ESP_ERR_INVALID_ARG;
  const auto& source = model.competitions[route].fallback;
  route_scratch.reset();
  const auto err = routeFetch(source, route, false, model, generation,
                              route_scratch, utc, false, false);
  if (err != ESP_OK) return err;
  for (unsigned i = 0; i < route_scratch.match_count; ++i) {
    const auto& m = route_scratch.matches[i];
    if (source.team.view() != m.home.id.data &&
        source.team.view() != m.away.id.data)
      continue;
    Selection entry;
    fixtureEntry(m, entry);
    lock();
    if (selection_state.count >= cfg::kSelectionEntries)
      selection_state.more = true;
    else
      selections[selection_state.count++] = entry;
    unlock();
  }
  return ESP_OK;
}
void runSelection(const settings::Model& model, uint32_t generation,
                  int64_t utc) {
  clearFailure();
  SelectionRequest request;
  lock();
  request = selection_request;
  selection_state.count = 0;
  selection_state.more = false;
  selection_state.error = {};
  selection_state.cause = Cause::kNone;
  unlock();
  if (request.fixtures) {
    finishSelection(runFixtures(model, generation, request.route, utc));
    return;
  }
  char url[384], comp[3 * cfg::kCompetitionIdMaxBytes + 1], season_text[33]{};
  char country[3 * cfg::kSelectionTextMaxBytes + 1];
  encode(request.country.empty() ? cfg::kSelectionCountryDefault
                                 : request.country.data,
         country);
  encode(request.competition.data, comp);
  memcpy(season_text, request.season.data, sizeof(season_text));
  const char* key = "";
  esp_err_t err = ESP_OK;
  // A club list needs the season in the URL at these two providers.
  if (request.teams && request.season.empty() &&
      (request.provider == cfg::Provider::kOpenLigaDb ||
       request.provider == cfg::Provider::kApiFootball)) {
    settings::Source current;
    current.provider = request.provider;
    err = currentSeason(current, comp, model, generation);
    if (err == ESP_OK)
      memcpy(season_text, current.season.data.data(), current.season.length);
  }
  if (request.provider == cfg::Provider::kOpenLigaDb) {
    if (request.teams)
      snprintf(url, sizeof(url),
               "https://api.openligadb.de/getavailableteams/%s/%s", comp,
               season_text);
    else {
      if (request.season.empty()) {
        const time_t stamp = utc;
        std::tm date{};
        gmtime_r(&stamp, &date);
        snprintf(season_text, sizeof(season_text), "%d",
                 date.tm_year + 1900 -
                     (date.tm_mon + 1 < cfg::kSeasonStartMonth ? 1 : 0));
      }
      snprintf(url, sizeof(url),
               "https://api.openligadb.de/getavailableleagues/%s",
               season_text);
    }
  } else if (request.provider == cfg::Provider::kApiFootball) {
    if (request.teams)
      snprintf(url, sizeof(url),
               "https://v3.football.api-sports.io/teams?league=%s&season=%s",
               comp, season_text);
    else
      snprintf(url, sizeof(url),
               "https://v3.football.api-sports.io/leagues?country=%s", country);
    key = "response";
  } else if (request.provider == cfg::Provider::kFootballData) {
    if (request.teams)
      snprintf(url, sizeof(url),
               "https://api.football-data.org/v4/competitions/%s/teams%s%s",
               comp, request.season.empty() ? "" : "?season=", season_text);
    else
      snprintf(url, sizeof(url),
               "https://api.football-data.org/v4/competitions");
    key = request.teams ? "teams" : "competitions";
  } else {
    if (request.teams)
      snprintf(url, sizeof(url),
               "https://site.api.espn.com/apis/site/v2/sports/soccer/%s/"
               "teams?limit=1000%s%s",
               comp, request.season.empty() ? "" : "&season=", season_text);
    else
      snprintf(url, sizeof(url),
               "https://site.api.espn.com/apis/site/v2/leagues/"
               "dropdown?lang=en&region=de&sport=soccer");
    key = request.teams ? "teams" : "leagues";
  }
  SelectContext context{&request};
  if (err == ESP_OK)
    err = get(url, key, request.provider, model, generation, selectionRow,
              &context, true);
  finishSelection(err);
}
template <class T>
void fill(Value<T>& primary, const Value<T>& secondary) {
  if ((!primary.valid() || primary.source == Source::kEstimated) &&
      confirmed(secondary.source))
    primary = secondary;
}
void fillScore(Score& primary, Score secondary, bool swapped) {
  if (swapped) std::swap(secondary.home, secondary.away);
  fill(primary.home, secondary.home);
  fill(primary.away, secondary.away);
}
void compose(Snapshot& primary, Snapshot& secondary,
             const settings::Model& model, uint8_t route, bool table_source) {
  const auto& fallback = model.competitions[route].fallback;
  for (unsigned i = 0; i < secondary.match_count; ++i)
    tagMatch(secondary, secondary.matches[i], fallback);
  for (const auto& pair : model.fixture_pairs) {
    if (!pair.active || pair.competition != route) continue;
    Match* a = nullptr;
    const Match* b = nullptr;
    for (unsigned i = 0; i < primary.match_count; ++i)
      if (primary.matches[i].route == route &&
          pair.primary_id.view() == primary.matches[i].id.data)
        a = &primary.matches[i];
    for (unsigned i = 0; i < secondary.match_count; ++i)
      if (pair.secondary_id.view() == secondary.matches[i].id.data)
        b = &secondary.matches[i];
    if (!a || !b) continue;
    if (!confirmed(a->phase_source) && confirmed(b->phase_source)) {
      a->phase = b->phase;
      a->phase_source = b->phase_source;
    }
    fill(a->minute, b->minute);
    fill(a->kickoff, b->kickoff);
    fillScore(a->principal, b->principal, pair.swapped);
    fillScore(a->halftime, b->halftime, pair.swapped);
    fillScore(a->regulation, b->regulation, pair.swapped);
    fillScore(a->extra_time, b->extra_time, pair.swapped);
    fillScore(a->shootout, b->shootout, pair.swapped);
    mergeEvents(primary, *a, secondary, *b, pair.swapped);
  }
  if (table_source && primary.table_count == 0 && secondary.table_count) {
    for (unsigned i = 0; i < secondary.table_count; ++i) {
      auto row = secondary.table[i];
      row.team.provider = fallback.provider;
      row.own = fallback.team.view() == row.team.id.data;
      for (auto* v :
           {&row.position, &row.played, &row.won, &row.drawn, &row.lost,
            &row.goals_for, &row.goals_against, &row.difference, &row.points})
        tag(*v, fallback.provider);
      primary.table[i] = row;
    }
    primary.table_count = secondary.table_count;
  }
}
esp_err_t routedFetch(uint8_t route, bool table_source,
                      const settings::Model& model, uint32_t generation,
                      Snapshot& target, int64_t utc) {
  const auto& competition = model.competitions[route];
  route_scratch.reset();
  auto err = routeFetch(competition.primary, route, table_source, model,
                        generation, route_scratch, utc);
  const bool primary_ok = err == ESP_OK;
  if (primary_ok && !appendRoute(target, route_scratch, competition.primary))
    return ESP_ERR_INVALID_SIZE;
  bool mapped = false;
  for (const auto& pair : model.fixture_pairs)
    mapped |= pair.active && pair.competition == route;
  const auto& fallback = competition.fallback;
  if (!competition.fallback_active ||
      (primary_ok && !mapped && (!table_source || target.table_count)))
    return err;
  route_scratch.reset();
  err = routeFetch(fallback, route, table_source, model, generation,
                   route_scratch, utc, false);
  if (err != ESP_OK) {
    if (primary_ok) {
      target.routing_notice.assign(esp_err_to_name(err));
      return ESP_OK;
    }
    return err;
  }
  if (primary_ok)
    compose(target, route_scratch, model, route, table_source);
  else {
    if (!appendRoute(target, route_scratch, fallback))
      return ESP_ERR_INVALID_SIZE;
    target.routing_notice.assign("fallback active");
  }
  return ESP_OK;
}
void reconcile(Snapshot& next, const Snapshot& old, int64_t utc) {
  for (unsigned i = 0; i < next.match_count; ++i) {
    auto& m = next.matches[i];
    if (m.route < next.route_fetched_ms.size())
      m.fetched_ms = next.route_fetched_ms[m.route];
    // First seen finished: now, or the expected end when the match was never
    // seen running (e.g. after a restart) (W22).
    bool seen_running = false, finished_before = false;
    for (unsigned j = 0; j < old.match_count; ++j) {
      const auto& prior = old.matches[j];
      if (m.route != prior.route || m.provider != prior.provider ||
          !(m.id == prior.id))
        continue;
      m.score_changed_ms = prior.score_changed_ms;
      if (!baseline && m.principal.valid() && prior.principal.valid() &&
          (m.principal.home.value != prior.principal.home.value ||
           m.principal.away.value != prior.principal.away.value))
        m.score_changed_ms = next.fetched_ms;
      seen_running = running(prior);
      finished_before = prior.finished_at.valid();
      if (finished_before) m.finished_at = prior.finished_at;
      if (!baseline && !m.events_truncated && !prior.events_truncated) {
        for (unsigned e = 0; e < prior.event_count; ++e) {
          const auto& previous = old.events[prior.events[e]];
          if (previous.removed) continue;
          bool present = false;
          for (unsigned n = 0; n < m.event_count; ++n) {
            const auto& current = next.events[m.events[n]];
            present |= current.provider == previous.provider &&
                       current.identity == previous.identity;
          }
          if (present) continue;
          if (m.event_count >= m.events.size() ||
              next.event_count >= next.events.size()) {
            m.events_truncated = true;
            break;
          }
          auto removed = previous;
          removed.removed = true;
          ++removed.revision;
          next.events[next.event_count] = removed;
          m.events[m.event_count++] = next.event_count++;
        }
      }
      break;
    }
    if (m.phase == Phase::kFinished && !finished_before) {
      int64_t at = utc;
      if (!seen_running && m.kickoff.valid())
        at = std::min(at, m.kickoff.value + cfg::kExpectedMatchMinutes * 60);
      m.finished_at = {at, Source::kEstimated};
    }
  }
}
// Replaces the worker's settings copy. Out of line, so the temporary copy
// lives in this frame only and not in the worker's frame during requests.
[[gnu::noinline]] void reload(settings::Model& model) {
  model = settings::current();
}
void worker(void*) {
  // Copied again only when one of the two settings generations changes. Saves
  // that keep the data generation (display-only changes) keep data and caches;
  // changed match windows or API budget only trigger a new refresh.
  uint32_t settings_seen = settings::generation();
  settings::Model model;
  reload(model);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    // Read before the copy: a save in between then interrupts this round.
    const auto generation = settings::dataGeneration();
    if (const auto seen = settings::generation();
        seen != settings_seen || generation != last_generation) {
      settings_seen = seen;
      const auto before = model.window_before, after = model.window_after,
                 budget = model.api_daily_budget;
      reload(model);
      if (model.window_before != before || model.window_after != after ||
          model.api_daily_budget != budget)
        requested.store(true);
    }
    // Only a clock step (first valid time, zone change, jump above
    // cfg::kTimeJumpThresholdS) needs a new baseline; the hourly SNTP resync
    // does not (W124).
    if (time_changed.exchange(false)) {
      const auto steps = timekeeping::status().step_count;
      if (steps != time_steps) {
        time_steps = steps;
        baseline = true;
        requested.store(true);
      }
    }
    const auto epoch = network::status().epoch;
    if (generation != last_generation) {
      lock();
      slots[published].reset();
      slots[published].generation = generation;
      error_routes = 0;
      unlock();
      for (auto& failure : route_failures) failure = {};
      last_generation = generation;
      for (auto& cache : caches) cache = {};
      baseline = true;
      requested.store(true);
    }
    if (epoch != last_epoch) {
      last_epoch = epoch;
      baseline = true;
      requested.store(true);
    }
    time_t utc = 0;
    const bool clock_valid = timekeeping::utcNow(utc);
    if (ota::trial()) continue;
    if ((!network::ready() || !clock_valid) && !model.demo) {
      // Under the mutex, like select(), so a new request cannot be lost.
      lock();
      if (selecting.load()) {
        selection_state.busy = false;
        selection_state.count = 0;
        selection_state.error.assign("network/time unavailable");
        selection_state.cause =
            network::ready() ? Cause::kTime : Cause::kNetwork;
        ++selection_state.revision;
        selecting.store(false);
      }
      unlock();
      continue;
    }
    if (utc / 86400 > day) {
      day = utc / 86400;
      lock();
      memset(day_requests, 0, sizeof(day_requests));
      unlock();
    }
    const auto now = timekeeping::monotonicMs();
    if (!selecting.load() && !requested.load() && now < due) continue;
    busy.store(true);
    if (selecting.load()) {
      runSelection(model, generation, utc);
      busy.store(false);
      continue;
    }
    // A requested refresh (owner, settings, time, network) also retries the
    // failed competitions at once.
    if (requested.exchange(false))
      for (auto& failure : route_failures) failure.retry_at = 0;
    const uint32_t api_before =
        requests[static_cast<unsigned>(cfg::Provider::kApiFootball)];
    auto& candidate = slots[1 - published];
    candidate.reset();
    candidate.generation = generation;
    // A failed competition keeps its earlier data, marked stale, and is
    // fetched again after its own retry delay while the others are published
    // (W81). A capacity overflow or an interrupted round (settings or network
    // changed) publishes nothing, as does a round in which every active
    // competition failed.
    esp_err_t err = ESP_OK;
    bool aborted = false;
    uint8_t failed = 0, fetched = 0;
    const RouteFailure* first_failure = nullptr;
    if (model.demo) {
      demo(candidate);
      candidate.route_fetched_ms.fill(timekeeping::monotonicMs());
    } else {
      bool first_active = true;  // supplies the league table
      for (unsigned i = 0; i < model.competitions.size() && !aborted; ++i) {
        if (!model.competitions[i].active) continue;
        candidate.configured = true;
        const bool table = first_active;
        first_active = false;
        auto& failure = route_failures[i];
        if (timekeeping::monotonicMs() >= failure.retry_at) {
          err = routedFetch(i, table, model, generation, candidate, utc);
          if (err == ESP_OK) {
            failure = {};
            fetched |= 1u << i;
            candidate.route_fetched_ms[i] = timekeeping::monotonicMs();
            continue;
          }
          aborted = err == ESP_ERR_INVALID_SIZE ||
                    settings::dataGeneration() != generation ||
                    network::status().epoch != epoch;
          failure.reason = reason(err);
          failure.cause = causeOf(err);
          failure.retry_at =
              timekeeping::monotonicMs() +
              (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_ALLOWED
                   ? cfg::kProviderRateRetryS
                   : cfg::kProviderErrorRetryS) *
                  1000LL;
        }
        failed |= 1u << i;
        if (!first_failure) first_failure = &failure;
        if (!aborted &&
            !carryRoute(candidate, slots[published], i, table, utc)) {
          err = ESP_ERR_INVALID_SIZE;
          aborted = true;
        }
      }
    }
    if (!aborted) err = ESP_OK;
    int64_t retry_at = 0;
    for (const auto& failure : route_failures)
      if (failure.retry_at && (retry_at == 0 || failure.retry_at < retry_at))
        retry_at = failure.retry_at;
    uint16_t interval = cfg::kPollIdleS;
    int64_t next_window_ms = cfg::kPollIdleS * 1000LL;
    const char* failed_text = nullptr;
    Cause failed_cause = Cause::kNone;
    const bool published_now = !aborted && (fetched || !failed) &&
                               settings::dataGeneration() == generation &&
                               (model.demo || network::status().epoch == epoch);
    if (published_now) {
      candidate.fetched_ms = timekeeping::monotonicMs();
      candidate.stale = failed != 0;
      if (first_failure) {
        candidate.error.assign(first_failure->reason);
        candidate.error_cause = first_failure->cause;
        candidate.retry_ms = retry_at;
      }
      lock();
      error_routes = failed;
      reconcile(candidate, slots[published], utc);
      if (conferenceWindow(candidate, utc, model.window_before,
                           model.window_after))
        interval = cfg::kPollPrematchS;
      for (unsigned i = 0; i < candidate.match_count; ++i) {
        auto& m = candidate.matches[i];
        if (m.kickoff.valid() && m.phase == Phase::kScheduled) {
          const int64_t until =
              m.kickoff.value - model.window_before * 60 - utc;
          if (until > 0)
            next_window_ms = std::min(next_window_ms, until * 1000);
        }
        if (matchWindow(m, utc, model.window_before, model.window_after))
          interval = std::min<uint16_t>(
              interval, m.phase == Phase::kFinished    ? cfg::kPollPostmatchS
                        : m.phase == Phase::kScheduled ? cfg::kPollPrematchS
                                                       : cfg::kPollLiveS);
      }
      const uint32_t cost =
          requests[static_cast<unsigned>(cfg::Provider::kApiFootball)] -
          api_before;
      const auto credits = budget::remaining(cfg::Provider::kApiFootball,
                                             model.api_daily_budget);
      if (cost && credits && interval != cfg::kPollIdleS) {
        int64_t coverage = 0;
        for (unsigned i = 0; i < candidate.match_count; ++i) {
          const auto& m = candidate.matches[i];
          if (m.kickoff.valid() &&
              matchWindow(m, utc, model.window_before, model.window_after))
            coverage = std::max(
                coverage,
                m.kickoff.value +
                    (cfg::kUnknownMatchEndMinutes + model.window_after) * 60 -
                    utc);
        }
        interval = static_cast<uint16_t>(std::min<int64_t>(
            cfg::kPollIdleS,
            std::max<int64_t>(interval,
                              (coverage * cost + credits - 1) / credits)));
      }
      candidate.revision = slots[published].revision + 1;
      published = 1 - published;
      unlock();
      baseline = false;
      events::post(events::Event::kDataUpdated);
    } else {
      if (aborted) {
        failed_text = reason(err);
        failed_cause = causeOf(err);
        interval = err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_ALLOWED
                       ? cfg::kProviderRateRetryS
                       : cfg::kProviderErrorRetryS;
      } else if (failed && !fetched) {
        // Every active competition failed; each keeps its own retry delay.
        failed_text = first_failure->reason;
        failed_cause = first_failure->cause;
      } else {
        const bool changed = settings::dataGeneration() != generation;
        err = stop(changed ? "settings changed" : "network changed",
                   ESP_ERR_INVALID_STATE,
                   changed ? Cause::kSettings : Cause::kNetwork);
        failed_text = reason(err);
        failed_cause = causeOf(err);
        interval = cfg::kProviderErrorRetryS;
      }
      lock();
      slots[published].stale = true;
      slots[published].error.assign(failed_text);
      slots[published].error_cause = failed_cause;
      error_routes = failed;
      unlock();
    }
    // The earliest retry of a failed competition also ends the wait.
    if (!aborted && retry_at)
      next_window_ms =
          std::min(next_window_ms, retry_at - timekeeping::monotonicMs());
    due = timekeeping::monotonicMs() +
          std::max<int64_t>(0, std::min<int64_t>(interval * 1000LL,
                                                 next_window_ms));
    if (!published_now) {
      lock();
      slots[published].retry_ms = due;
      unlock();
    }
    busy.store(false);
  }
}
}  // namespace
esp_err_t init() {
  mutex = xSemaphoreCreateMutexStatic(&mutex_buffer);
  const auto budget_error = budget::init();
  if (budget_error != ESP_OK)
    ESP_LOGE("provider", "daily budget unavailable: %s",
             esp_err_to_name(budget_error));
  const auto subscription = events::subscribe(
      events::Event::kTimeChanged,
      [](events::Event, uint32_t, void*) { time_changed.store(true); },
      nullptr);
  if (subscription != ESP_OK) return subscription;
  return xTaskCreateStatic(worker, "provider", cfg::kProviderStackBytes,
                           nullptr, 3, stack, &task_buffer)
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}
void refresh() { requested.store(true); }
Status status() {
  Status s{};
  if (mutex == nullptr) {
    s.error.assign("not started");
    s.cause = Cause::kNotStarted;
    return s;
  }
  lock();
  const auto& data = slots[published];
  s.revision = data.revision;
  s.generation = data.generation;
  memcpy(s.requests, requests, sizeof(requests));
  s.fetched_ms = oldestFetch(data);
  s.matches = data.match_count;
  s.rows = data.table_count;
  s.stale = data.stale || (s.fetched_ms > 0 &&
                           timekeeping::monotonicMs() - s.fetched_ms >
                               cfg::kDataStaleS * 1000LL);
  s.error = data.error;
  s.cause = data.error_cause;
  s.routing_notice = data.routing_notice;
  s.error_routes = error_routes;
  unlock();
  s.busy = busy.load();
  return s;
}
void read(Reader reader, void* context) {
  if (mutex == nullptr) return;
  lock();
  reader(slots[published], context);
  unlock();
}
namespace {
esp_err_t start(const SelectionRequest& request) {
  if (mutex == nullptr) return ESP_ERR_INVALID_STATE;
  lock();
  // The request is stored before the worker can see the flag.
  const bool running = selecting.load();
  if (!running) {
    selection_request = request;
    selection_state.busy = true;
    selection_state.fixtures = request.fixtures;
    selection_state.route = request.route;
    selection_state.count = 0;
    selection_state.more = false;
    selection_state.error = {};
    selection_state.cause = Cause::kNone;
    selecting.store(true);
  }
  unlock();
  return running ? ESP_ERR_NOT_FINISHED : ESP_OK;
}
}  // namespace
esp_err_t select(const SelectionRequest& request) {
  auto list = request;
  list.fixtures = false;
  list.route = 0;
  return start(list);
}
esp_err_t selectFixtures(uint8_t route) {
  const auto model = settings::current();
  if (route >= model.competitions.size() ||
      !model.competitions[route].fallback.complete())
    return ESP_ERR_INVALID_ARG;
  SelectionRequest request;
  request.fixtures = true;
  request.route = route;
  return start(request);
}
SelectionStatus selectionStatus() {
  lock();
  auto s = selection_state;
  unlock();
  return s;
}
bool selectionAt(uint16_t index, uint32_t revision, Selection& out) {
  lock();
  const bool ok = !selection_state.busy &&
                  revision == selection_state.revision &&
                  index < selection_state.count;
  if (ok) out = selections[index];
  unlock();
  return ok;
}
Overview overview(int64_t utc) {
  Overview result{};
  if (mutex == nullptr) return result;
  lock();
  const auto& data = slots[published];
  const Match* best = nullptr;
  bool best_live = false;
  int64_t best_distance = 0;
  for (unsigned i = 0; i < data.match_count; ++i) {
    const auto& m = data.matches[i];
    if (m.route < cfg::kCompetitionMax) {
      auto& info = result.routes[m.route];
      if (info.matches == 0) info.provider = m.provider;
      if (info.matches < UINT8_MAX) ++info.matches;
    }
    if (!m.own) continue;
    const bool live = m.phase == Phase::kLive || m.phase == Phase::kHalfTime ||
                      m.phase == Phase::kExtraTime ||
                      m.phase == Phase::kPenalties;
    // Unknown kickoffs rank last.
    const int64_t distance =
        m.kickoff.valid() ? std::llabs(utc - m.kickoff.value) : INT64_MAX;
    if (best == nullptr || (live && !best_live) ||
        (live == best_live && distance < best_distance)) {
      best = &m;
      best_live = live;
      best_distance = distance;
    }
  }
  if (best != nullptr) {
    auto& own = result.own;
    own.present = true;
    own.route = best->route;
    own.phase = best->phase;
    own.kickoff = best->kickoff.valid() ? best->kickoff.value : 0;
    if (best->principal.valid()) {
      own.home_goals = best->principal.home.value;
      own.away_goals = best->principal.away.value;
    }
    if (best->minute.valid()) own.minute = best->minute.value;
    own.home.assign(best->home.name.data);
    own.away.assign(best->away.name.data);
  }
  unlock();
  return result;
}
uint16_t publishedFixtureCount(uint8_t route, cfg::Provider provider,
                               uint32_t& revision) {
  revision = 0;
  if (mutex == nullptr) return 0;
  uint16_t count = 0;
  lock();
  const auto& data = slots[published];
  revision = data.revision;
  for (unsigned i = 0; i < data.match_count; ++i) {
    const auto& m = data.matches[i];
    if (m.route == route && m.own && m.provider == provider) ++count;
  }
  unlock();
  return count;
}
bool publishedFixtureAt(uint8_t route, cfg::Provider provider, uint16_t index,
                        uint32_t revision, Selection& out) {
  if (mutex == nullptr) return false;
  bool found = false;
  lock();
  const auto& data = slots[published];
  if (data.revision == revision) {
    uint16_t seen = 0;
    for (unsigned i = 0; i < data.match_count && !found; ++i) {
      const auto& m = data.matches[i];
      if (m.route != route || !m.own || m.provider != provider) continue;
      if (seen++ == index) {
        fixtureEntry(m, out);
        found = true;
      }
    }
  }
  unlock();
  return found;
}
}  // namespace football
