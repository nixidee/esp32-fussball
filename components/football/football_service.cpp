#include "football_service.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

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
uint32_t requests[4]{}, day_requests[4]{};
int64_t last_request[4]{}, day = 0, due = 0;
uint32_t last_generation = 0, last_epoch = 0;
struct RouteCache {
  char season[17]{}, change[64]{};
  int round = 0;
  int64_t full_at = 0, season_at = 0;
};
RouteCache caches[cfg::kRouteCount];
bool baseline = true;
const char* str(ArduinoJson::JsonVariantConst v) {
  return v.is<const char*>() ? v.as<const char*>() : "";
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
bool allowed(cfg::Provider p, const settings::Model& model) {
  if (p == cfg::Provider::kEspn && !model.espn_opt_in) return false;
  if (p == cfg::Provider::kApiFootball && model.api_football_key.empty())
    return false;
  if (p == cfg::Provider::kFootballData && model.football_data_key.empty())
    return false;
  return true;
}
esp_err_t get(const char* url, const char* array, cfg::Provider p,
              const settings::Model& m, uint32_t generation,
              JsonConsumer consumer, void* context, bool selection = false) {
  if (!allowed(p, m)) return ESP_ERR_INVALID_STATE;
  const auto index = static_cast<unsigned>(p);
  const auto now = timekeeping::monotonicMs();
  const unsigned daily_limit = p == cfg::Provider::kApiFootball
                                   ? m.api_daily_budget
                                   : cfg::kProviderDefaultBudget;
  const int64_t remaining =
      cfg::kProviderMinIntervalMs[index] - (now - last_request[index]);
  const uint32_t epoch = network::status().epoch;
  const int64_t ready_at =
      now + std::max<int64_t>(0, last_request[index] > 0 ? remaining : 0);
  while (timekeeping::monotonicMs() < ready_at) {
    if (settings::generation() != generation || !network::ready() ||
        network::status().epoch != epoch)
      return ESP_ERR_INVALID_STATE;
    vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
  }
  if (settings::generation() != generation || !network::ready())
    return ESP_ERR_INVALID_STATE;
  const int64_t deadline =
      timekeeping::monotonicMs() + cfg::kOperationDeadlineMs;
  while (!network::tryHeavy()) {
    if (settings::generation() != generation || !network::ready() ||
        network::status().epoch != epoch ||
        timekeeping::monotonicMs() >= deadline)
      return ESP_ERR_TIMEOUT;
    vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
  }
  const auto admitted =
      budget::admit(p, static_cast<uint32_t>(day), daily_limit);
  if (admitted != ESP_OK) {
    network::releaseHeavy();
    return admitted;
  }
  last_request[index] = timekeeping::monotonicMs();
  lock();
  ++requests[index];
  ++day_requests[index];
  unlock();
  const auto err =
      fetchJson(url, array, p, m, generation, consumer, context, selection);
  network::releaseHeavy();
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
void tagMatch(Snapshot& data, Match& m, const settings::Route& route) {
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
bool appendMatch(Snapshot& target, const Snapshot& from, const Match& source,
                 bool primary_events_only = false) {
  if (target.match_count >= target.matches.size()) return false;
  auto& match = target.matches[target.match_count++];
  match = source;
  match.event_count = 0;
  for (unsigned i = 0; i < source.event_count; ++i) {
    if (primary_events_only &&
        from.events[source.events[i]].provider != source.provider)
      continue;
    if (target.event_count >= target.events.size()) {
      match.events_truncated = true;
      break;
    }
    target.events[target.event_count] = from.events[source.events[i]];
    match.events[match.event_count++] = target.event_count++;
  }
  return true;
}
bool appendRoute(Snapshot& target, Snapshot& from,
                 const settings::Route& route) {
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
  auto* route = static_cast<settings::Route*>(context);
  if (row["leagueSeason"].is<int>()) {
    char b[17];
    snprintf(b, sizeof(b), "%d", row["leagueSeason"].as<int>());
    return route->season.assign(b);
  }
  return false;
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
esp_err_t routeFetch(settings::Route route, uint8_t index,
                     const settings::Model& model, uint32_t generation,
                     Snapshot& target, int64_t utc, bool primary = true) {
  char url[384], competition[33]{}, season_text[17]{};
  memcpy(competition, route.competition.data.data(), route.competition.length);
  memcpy(season_text, route.season.data.data(), route.season.length);
  const auto p = route.provider;
  char own_team[33]{};
  memcpy(own_team, route.team.data.data(), route.team.length);
  Mapping mapping{&target, p, index, utc, false, {own_team, 0}};
  esp_err_t err;
  if (p == cfg::Provider::kOpenLigaDb) {
    auto& cache = caches[index];
    if (primary && route.season.empty() && cache.season[0] &&
        timekeeping::monotonicMs() - cache.season_at < cfg::kPollIdleS * 1000LL)
      route.season.assign(cache.season);
    if (route.season.empty()) {
      snprintf(url, sizeof(url), "https://api.openligadb.de/getmatchdata/%s",
               competition);
      err = get(url, "", p, model, generation, season, &route, true);
      if (err != ESP_OK || route.season.empty())
        return ESP_ERR_INVALID_RESPONSE;
      memcpy(season_text, route.season.data.data(), route.season.length);
      if (primary) {
        snprintf(cache.season, sizeof(cache.season), "%s", season_text);
        cache.season_at = timekeeping::monotonicMs();
      }
    } else
      memcpy(season_text, route.season.data.data(), route.season.length);
    // Cheap change endpoint suppresses full refreshes of the current round.
    int round = 0;
    snprintf(url, sizeof(url), "https://api.openligadb.de/getcurrentgroup/%s",
             competition);
    err = get(url, nullptr, p, model, generation, group, &round);
    if (err != ESP_OK) return err;
    mapping.filter.round = round;
    char timestamp[64]{};
    snprintf(url, sizeof(url),
             "https://api.openligadb.de/getlastchangedate/%s/%s/%d",
             competition, season_text, round);
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
      if (index == 0 && prior.table_count &&
          prior.table[0].team.provider == p) {
        target.table = prior.table;
        target.table_count = prior.table_count;
      }
      return ESP_OK;
    }
    // A complete season stream covers catch-up fixtures outside the current
    // round. Only horizon-relevant fixtures survive into fixed storage.
    snprintf(url, sizeof(url), "https://api.openligadb.de/getmatchdata/%s/%s",
             competition, season_text);
    err = get(url, "", p, model, generation, consume, &mapping, true);
    if (index == 0 && err == ESP_OK) {
      mapping.table = true;
      snprintf(url, sizeof(url), "https://api.openligadb.de/getbltable/%s/%s",
               competition, season_text);
      err = get(url, "", p, model, generation, consume, &mapping);
    }
    if (err == ESP_OK && primary) {
      snprintf(cache.change, sizeof(cache.change), "%s", timestamp);
      cache.round = round;
      cache.full_at = timekeeping::monotonicMs();
    }
  } else if (p == cfg::Provider::kApiFootball) {
    if (route.season.empty()) return ESP_ERR_INVALID_ARG;
    char from[16], to[16];
    dates(utc, from, to, false);
    snprintf(url, sizeof(url),
             "https://v3.football.api-sports.io/"
             "fixtures?league=%s&season=%s&from=%s&to=%s",
             competition, season_text, from, to);
    err = get(url, "response", p, model, generation, consume, &mapping);
    if (index == 0 && err == ESP_OK) {
      mapping.table = true;
      snprintf(
          url, sizeof(url),
          "https://v3.football.api-sports.io/standings?league=%s&season=%s",
          competition, season_text);
      err = get(url, "response", p, model, generation, consume, &mapping);
    }
    if (err == ESP_OK)
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
             competition, from, to,
             route.season.empty() ? "" : "&season=", season_text);
    err = get(url, "matches", p, model, generation, consume, &mapping);
    if (index == 0 && err == ESP_OK) {
      mapping.table = true;
      snprintf(url, sizeof(url),
               "https://api.football-data.org/v4/competitions/%s/standings%s%s",
               competition,
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
             competition, today,
             route.season.empty() ? "" : "&season=", season_text);
    err = get(url, "events", p, model, generation, consume, &mapping);
    // Soccer scoreboard range requests returned HTTP 400 during contract
    // research. The team's fixture schedule supplies upcoming own matches.
    if (err == ESP_OK) {
      snprintf(
          url, sizeof(url),
          "https://site.api.espn.com/apis/site/v2/sports/soccer/%s/teams/%s/"
          "schedule?fixture=true%s%s",
          competition, own_team,
          route.season.empty() ? "" : "&season=", season_text);
      err = get(url, "events", p, model, generation, consume, &mapping, true);
    }
    if (err == ESP_OK) {
      snprintf(
          url, sizeof(url),
          "https://site.api.espn.com/apis/site/v2/sports/soccer/%s/teams/%s/"
          "schedule%s%s",
          competition, own_team,
          route.season.empty() ? "" : "?season=", season_text);
      err = get(url, "events", p, model, generation, consume, &mapping, true);
    }
    if (index == 0 && err == ESP_OK) {
      mapping.table = true;
      snprintf(url, sizeof(url),
               "https://site.web.api.espn.com/apis/v2/sports/soccer/%s/"
               "standings%s%s",
               competition,
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
bool selectionRow(ArduinoJson::JsonVariantConst row, void* context) {
  auto& c = *static_cast<SelectContext*>(context);
  const auto& request = *c.request;
  Selection s;
  const auto p = request.provider;
  if (p == cfg::Provider::kOpenLigaDb) {
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
    for (char& ch : hay)
      if (ch >= 'A' && ch <= 'Z') ch += 32;
    for (char& ch : needle)
      if (ch >= 'A' && ch <= 'Z') ch += 32;
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
void runSelection(const settings::Model& model, uint32_t generation) {
  SelectionRequest request;
  lock();
  request = selection_request;
  selection_state.count = 0;
  selection_state.more = false;
  selection_state.error = {};
  unlock();
  char url[384], comp[33]{}, season_text[33]{};
  char country[145]{};
  const char* country_name = request.country.empty()
                                 ? cfg::kSelectionCountryDefault
                                 : request.country.data;
  unsigned country_at = 0;
  for (const auto* cursor = country_name; *cursor; ++cursor) {
    const auto c = static_cast<unsigned char>(*cursor);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-')
      country[country_at++] = c;
    else {
      snprintf(country + country_at, 4, "%%%02X", c);
      country_at += 3;
    }
  }
  memcpy(comp, request.competition.data, sizeof(comp));
  memcpy(season_text, request.season.data, sizeof(season_text));
  const char* key = "";
  esp_err_t err = ESP_OK;
  if (request.provider == cfg::Provider::kOpenLigaDb) {
    if (request.teams && request.season.empty()) {
      settings::Route current;
      snprintf(url, sizeof(url), "https://api.openligadb.de/getmatchdata/%s",
               comp);
      err = get(url, "", request.provider, model, generation, season, &current,
                true);
      if (err == ESP_OK && current.season.empty())
        err = ESP_ERR_INVALID_RESPONSE;
      if (err == ESP_OK) {
        memcpy(season_text, current.season.data.data(), current.season.length);
      }
    }
    if (request.teams)
      snprintf(url, sizeof(url),
               "https://api.openligadb.de/getavailableteams/%s/%s", comp,
               season_text);
    else
      snprintf(url, sizeof(url),
               "https://api.openligadb.de/getavailableleagues");
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
  lock();
  if (err != ESP_OK) {
    selection_state.error.assign(esp_err_to_name(err));
    selection_state.count = 0;
  }
  selection_state.busy = false;
  ++selection_state.revision;
  unlock();
  selecting.store(false);
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
bool isGoal(EventType type) {
  return type == EventType::kGoal || type == EventType::kOwnGoal ||
         type == EventType::kPenaltyGoal || type == EventType::kPenaltyMissed;
}
void compose(Snapshot& primary, Snapshot& secondary,
             const settings::Model& model, uint8_t route) {
  const auto& fallback = model.fallback_routes[route];
  for (unsigned i = 0; i < secondary.match_count; ++i)
    tagMatch(secondary, secondary.matches[i], fallback);
  for (const auto& pair : model.fixture_mappings) {
    if (!pair.enabled || pair.route != route) continue;
    Match* a = nullptr;
    const Match* b = nullptr;
    for (unsigned i = 0; i < primary.match_count; ++i)
      if (primary.matches[i].route == route &&
          pair.primary.view() == primary.matches[i].id.data)
        a = &primary.matches[i];
    for (unsigned i = 0; i < secondary.match_count; ++i)
      if (pair.secondary.view() == secondary.matches[i].id.data)
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
    bool primary_goals = false;
    for (unsigned i = 0; i < a->event_count; ++i)
      primary_goals |= isGoal(primary.events[a->events[i]].type);
    for (unsigned i = 0; i < b->event_count; ++i) {
      const auto& event = secondary.events[b->events[i]];
      if (primary_goals && isGoal(event.type)) continue;
      if (a->event_count >= a->events.size() ||
          primary.event_count >= primary.events.size()) {
        a->events_truncated = true;
        break;
      }
      primary.events[primary.event_count] = event;
      if (pair.swapped)
        std::swap(primary.events[primary.event_count].after.home,
                  primary.events[primary.event_count].after.away);
      a->events[a->event_count++] = primary.event_count++;
    }
    a->events_truncated |= b->events_truncated;
  }
  if (route == 0 && primary.table_count == 0 && secondary.table_count) {
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
esp_err_t routedFetch(uint8_t route, const settings::Model& model,
                      uint32_t generation, Snapshot& target, int64_t utc) {
  route_scratch.reset();
  auto err = routeFetch(model.routes[route], route, model, generation,
                        route_scratch, utc);
  const bool primary_ok = err == ESP_OK;
  if (primary_ok && !appendRoute(target, route_scratch, model.routes[route]))
    return ESP_ERR_INVALID_SIZE;
  bool mapped = false;
  for (const auto& pair : model.fixture_mappings)
    mapped |= pair.enabled && pair.route == route;
  const auto& fallback = model.fallback_routes[route];
  if (!fallback.enabled ||
      (primary_ok && !mapped && (route != 0 || target.table_count)))
    return err;
  route_scratch.reset();
  err =
      routeFetch(fallback, route, model, generation, route_scratch, utc, false);
  if (err != ESP_OK) {
    if (primary_ok) {
      target.routing_notice.assign(esp_err_to_name(err));
      return ESP_OK;
    }
    return err;
  }
  if (primary_ok)
    compose(target, route_scratch, model, route);
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
    m.fetched_ms = next.fetched_ms;
    if (m.phase == Phase::kFinished) m.finished_at = {utc, Source::kEstimated};
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
      if (prior.finished_at.valid()) m.finished_at = prior.finished_at;
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
  }
}
void worker(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    const auto generation = settings::generation();
    if (time_changed.exchange(false)) {
      baseline = true;
      requested.store(true);
    }
    const auto epoch = network::status().epoch;
    if (generation != last_generation) {
      lock();
      slots[published].reset();
      slots[published].generation = generation;
      unlock();
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
    const auto model = settings::current();
    time_t utc = 0;
    const bool clock_valid = timekeeping::utcNow(utc);
    if (ota::trial()) continue;
    if ((!network::ready() || !clock_valid) && !model.demo) {
      if (selecting.exchange(false)) {
        lock();
        selection_state.busy = false;
        selection_state.count = 0;
        selection_state.error.assign("network/time unavailable");
        ++selection_state.revision;
        unlock();
      }
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
      runSelection(model, generation);
      busy.store(false);
      continue;
    }
    requested.store(false);
    const uint32_t api_before =
        requests[static_cast<unsigned>(cfg::Provider::kApiFootball)];
    auto& candidate = slots[1 - published];
    candidate.reset();
    candidate.generation = generation;
    esp_err_t err = ESP_OK;
    if (model.demo)
      demo(candidate);
    else
      for (unsigned i = 0; i < model.routes.size(); ++i) {
        if (!model.routes[i].enabled) continue;
        candidate.configured = true;
        err = routedFetch(i, model, generation, candidate, utc);
        if (err != ESP_OK) break;
      }
    uint16_t interval = cfg::kPollIdleS;
    int64_t next_window_ms = cfg::kPollIdleS * 1000LL;
    if (err == ESP_OK && settings::generation() == generation &&
        (model.demo || network::status().epoch == epoch)) {
      candidate.fetched_ms = timekeeping::monotonicMs();
      candidate.stale = false;
      lock();
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
      if (err == ESP_OK) err = ESP_ERR_INVALID_STATE;
      lock();
      slots[published].stale = true;
      slots[published].error.assign(esp_err_to_name(err));
      unlock();
      interval = err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_ALLOWED
                     ? cfg::kProviderRateRetryS
                     : cfg::kProviderErrorRetryS;
    }
    due = timekeeping::monotonicMs() +
          std::min<int64_t>(interval * 1000LL, next_window_ms);
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
    return s;
  }
  lock();
  const auto& data = slots[published];
  s.revision = data.revision;
  s.generation = data.generation;
  memcpy(s.requests, requests, sizeof(requests));
  s.fetched_ms = data.fetched_ms;
  s.matches = data.match_count;
  s.rows = data.table_count;
  s.stale = data.stale || (data.fetched_ms > 0 &&
                           timekeeping::monotonicMs() - data.fetched_ms >
                               cfg::kDataStaleS * 1000LL);
  s.error = data.error;
  s.routing_notice = data.routing_notice;
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
esp_err_t select(const SelectionRequest& request) {
  if (mutex == nullptr) return ESP_ERR_INVALID_STATE;
  bool expected = false;
  if (!selecting.compare_exchange_strong(expected, true))
    return ESP_ERR_INVALID_STATE;
  lock();
  selection_request = request;
  selection_state.busy = true;
  unlock();
  return ESP_OK;
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
}  // namespace football
