#include "football_service.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <sys/stat.h>

#include "dirent.h"
#include "esp_log.h"
#include "event_bus.h"
#include "file_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "image_service.h"
#include "network_service.h"
#include "ota_service.h"
#include "provider_client.h"
#include "provider_plan.h"
#include "request_budget.h"
#include "request_scheduler.h"
#include "settings_store.h"
#include "time_service.h"
namespace football {
namespace {
// Provider worker (docs/DATA_FLOW.md "Request executor"): one loop asks the
// request scheduler which job runs next, lets the provider plan build its
// request, streams the response into the compact store and sleeps until the
// scheduler's next wake time. The store is written only by this worker,
// always under the mutex; it reads it without the mutex. Readers (display,
// Web UI) hold the mutex for the whole read.
StaticSemaphore_t mutex_buffer;
SemaphoreHandle_t mutex = nullptr;
StaticTask_t task_buffer;
StackType_t stack[cfg::kProviderStackBytes / sizeof(StackType_t)];
std::atomic<bool> requested{false}, selecting{false}, busy{false};
std::atomic<bool> time_changed{false};
SelectionRequest selection_request;
Selection selections[cfg::kSelectionEntries];
SelectionStatus selection_state{};

MatchStore store;
view::Shown shown;
// Worker only.
plan::Planner planner;
schedule::Scheduler scheduler;
schedule::Inputs inputs;
plan::Request request;
// The worker's copy of the competition store (index = route = scheduler
// slot), refreshed with its settings copy. Static: 2.5 KB would not fit the
// worker's stack.
settings::Competitions routes;

// Per route (mutex-protected; written by the worker). 'key' hashes the
// competition's source: a changed key drops its stored data.
struct Slot {
  uint8_t id = 0;
  bool active = false;
  uint32_t key = 0;
  // Monotonic ms of the last successful request of any data type, and of
  // the last successful match request (fixtures or live).
  int64_t fetched_ms = 0, matches_ms = 0;
  // The source's last request failed: why, the provider asked and its HTTP
  // error status (0 = none). 'unmaintained': the last successful match
  // request still held an old match without a result (OQ-281).
  bool failed = false, unmaintained = false;
  Cause cause = Cause::kNone;
  cfg::Provider provider = cfg::kProviderDefault;
  uint16_t http = 0;
  // Kept data (worker only): UTC second each kept data type last completed (0 =
  // never), new data since the last write, whether the competition file holds
  // this slot's data and its size, the monotonic ms of the last write attempt,
  // and the UTC second a loaded file was written while no request has succeeded
  // since (0 = none).
  std::array<int64_t, persist::kKeptTypes> done{};
  bool dirty = false, on_file = false;
  uint32_t file_bytes = 0;
  int64_t written_ms = 0, restored_utc = 0;
};
std::array<Slot, cfg::kCompetitionMax> slots{};
// 'demo_competition': the competition the demo data fills.
uint8_t route_count = 0, demo_competition = 0;
bool configured = false, demo_mode = false;
uint32_t revision = 0, published_generation = 0;
uint32_t requests[cfg::kProviderCount]{};
// Bit i set: route i failed. 'states': what publish() derived from the
// slots for the readers (mutex-protected).
uint8_t error_routes = 0;
CompetitionStates states{};
int64_t retry_ms = 0, due_ms = 0;
Cadence cadence_now = Cadence::kNone;
// UTC day of the daily ledgers (worker only).
uint32_t day = 0;
// The owner's short names: read by the views under the mutex, changed under
// the mutex by the holder of the heavy-operation token, which alone writes
// the short-name file.
persist::ShortNames names;
// Size of the short-name file (0 = none); changed by the token holder.
std::atomic<uint32_t> names_bytes{0};
// images::format() wiped the football files (filesLost()).
std::atomic<bool> files_lost{false};
// Worker only: competition files of other data to delete, loaded data to
// hand to the scheduler with the first valid clock, the short-name file to
// write again.
bool sweep_pending = false, seed_pending = false, names_pending = false;

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
Clock nowClock() {
  time_t utc = 0;
  timekeeping::utcNow(utc);
  return {static_cast<int64_t>(utc),
          static_cast<uint32_t>(timekeeping::monotonicMs() / 1000)};
}
uint32_t fnv(uint32_t hash, std::string_view bytes) {
  for (const char c : bytes) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  // Separator, so "ab"+"c" and "a"+"bc" differ.
  hash ^= 0xffu;
  return hash * 16777619u;
}
uint32_t fnv(uint32_t hash, uint32_t value) {
  char bytes[4];
  memcpy(bytes, &value, sizeof(bytes));
  return fnv(hash, std::string_view(bytes, sizeof(bytes)));
}
uint32_t sourceHash(uint32_t hash, const settings::Source& s) {
  hash = fnv(hash, static_cast<uint32_t>(s.provider));
  hash = fnv(hash, s.competition.view());
  hash = fnv(hash, s.season.view());
  return fnv(hash, s.team.view());
}
// What the stored data of a competition depends on: its source.
uint32_t routeKey(const settings::Competition& e) {
  return sourceHash(fnv(2166136261u, e.id), e.primary);
}
// Per provider, what its access depends on: its key (API-Football,
// football-data.org) or the ESPN opt-in. A change drops the data of the
// competitions that use that provider.
using Access = std::array<uint32_t, cfg::kProviderCount>;
Access accessOf(const settings::Model& m) {
  Access out{};
  out[static_cast<std::size_t>(cfg::Provider::kApiFootball)] =
      fnv(2166136261u, m.api_football_key.view());
  out[static_cast<std::size_t>(cfg::Provider::kFootballData)] =
      fnv(2166136261u, m.football_data_key.view());
  out[static_cast<std::size_t>(cfg::Provider::kEspn)] = m.espn_opt_in;
  return out;
}
// Bit per provider whose access differs between 'a' and 'b'.
uint8_t changedProviders(const Access& a, const Access& b) {
  uint8_t mask = 0;
  for (std::size_t p = 0; p < cfg::kProviderCount; ++p)
    if (a[p] != b[p]) mask |= static_cast<uint8_t>(1u << p);
  return mask;
}
bool usesAny(const settings::Competition& e, uint8_t providers) {
  return !e.primary.competition.empty() &&
         ((1u << static_cast<unsigned>(e.primary.provider)) & providers) != 0;
}
const Slot* slotOf(uint8_t competition) {
  for (uint8_t i = 0; i < route_count; ++i)
    if (slots[i].id == competition) return &slots[i];
  return nullptr;
}

// Why the last get() failed. Provider task only; the code pairs the cause
// with the error it explains.
Cause failure_cause = Cause::kNone;
esp_err_t failure_code = ESP_OK;
// HTTP status of the last get() that failed after sending (0 = none).
int failure_status = 0;
// The last get() sent its request (a failure came from the provider or the
// connection, not from a local refusal).
bool sent = false;
void clearFailure() {
  failure_cause = Cause::kNone;
  failure_code = ESP_OK;
  failure_status = 0;
  sent = false;
}
esp_err_t stop(esp_err_t err, Cause cause) {
  failure_cause = cause;
  failure_code = err;
  return err;
}
// The cause recorded for this error, else one derived from the plain error
// code (failures outside get()).
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
// The HTTP error status behind 'err' (0 = none: refused before sending, no
// response, or a 2xx response that could not be used).
uint16_t httpOf(esp_err_t err) {
  if (!sent || failure_code != err) return 0;
  return failure_status >= 300 && failure_status <= UINT16_MAX
             ? static_cast<uint16_t>(failure_status)
             : 0;
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
// The last get() stopped because settings or the network changed.
bool interruption(esp_err_t err) {
  return failure_code == err && (failure_cause == Cause::kSettings ||
                                 failure_cause == Cause::kNetwork);
}
// One provider request the scheduler has admitted (a job, or a step of the
// owner's selection that asked admissibleAt()). It never waits: a busy heavy
// token, a used daily budget or changed settings refuse it before anything is
// sent, and the caller reschedules. The scheduler counts the request when it
// is really sent (sent()); 'sent_s' keeps that second for finished(). 'body'
// caps the response body (plan::Request::body).
uint32_t sent_s = 0;
esp_err_t get(const char* url, const char* array, bool optional,
              cfg::Provider p, const settings::Model& m, uint32_t generation,
              uint32_t epoch, JsonConsumer consumer, void* context,
              const bool* enough = nullptr,
              std::size_t body = cfg::kProviderBodyBytes) {
  clearFailure();
  // One log line per provider request, sent or not. Keys travel in headers,
  // never in the URL.
  const auto skip = [url](const char* why, esp_err_t err, Cause cause) {
    ESP_LOGW("provider", "not sent: %s (%s)", url, why);
    return stop(err, cause);
  };
  if (!settings::providerUsable(p, m))
    return skip("provider not enabled", ESP_ERR_INVALID_STATE,
                Cause::kProviderOff);
  if (const auto stopped = interrupted(generation, epoch))
    return skip(stopped.why, ESP_ERR_INVALID_STATE, stopped.cause);
  // Another heavy operation (OTA, upload, image work) runs: the request
  // waits for the next pass. Debug level, since it repeats until the token
  // is free.
  if (!network::tryHeavy()) {
    ESP_LOGD("provider", "not sent: %s (operation busy)", url);
    return stop(ESP_ERR_TIMEOUT, Cause::kBusy);
  }
  const auto index = static_cast<unsigned>(p);
  const auto& global = m.global_limit;
  // The global cap is checked before the provider's counter takes the
  // request, so a refusal there costs the provider nothing.
  if (global.daily && budget::remainingGlobal(day, global.daily) == 0) {
    network::releaseHeavy();
    return skip("daily budget used", ESP_ERR_NOT_ALLOWED, Cause::kBudget);
  }
  auto admitted = budget::admit(p, day, m.providers[index].limit.daily);
  if (admitted == ESP_OK) admitted = budget::admitGlobal(day, global.daily);
  if (admitted != ESP_OK) {
    network::releaseHeavy();
    if (admitted == ESP_ERR_NOT_ALLOWED)
      return skip("daily budget used", admitted, Cause::kBudget);
    return skip("budget unavailable", admitted, Cause::kCounter);
  }
  sent_s = static_cast<uint32_t>(timekeeping::monotonicMs() / 1000);
  scheduler.sent(p, sent_s);
  lock();
  ++requests[index];
  unlock();
  sent = true;
  FetchResult result;
  const int64_t started = timekeeping::monotonicMs();
  const auto err = fetchJson(url, array, p, m, generation, consumer, context,
                             body, &result, optional, enough);
  network::releaseHeavy();
  const auto elapsed =
      static_cast<long long>(timekeeping::monotonicMs() - started);
  if (err == ESP_OK) {
    ESP_LOGI("provider", "%s: HTTP %d, %u B, %lld ms", url, result.status,
             static_cast<unsigned>(result.bytes), elapsed);
  } else {
    ESP_LOGW("provider", "%s: HTTP %d, %u B, %lld ms, %s, stage %s, socket %d",
             url, result.status, static_cast<unsigned>(result.bytes), elapsed,
             esp_err_to_name(err), result.stage, result.socket_error);
    if (const auto stopped = interrupted(generation, epoch))
      stop(err, stopped.cause);
    else {
      failure_cause = fetchCause(err, result.status);
      failure_code = err;
      failure_status = result.status;
    }
  }
  return err;
}
// fetchJson consumer of the provider plan: each element is mapped into the
// store under the mutex.
bool consumeLocked(ArduinoJson::JsonVariantConst element, void*) {
  lock();
  const bool ok = plan::Planner::consume(element, &planner);
  unlock();
  return ok;
}

// Under the mutex: the shown sources and the competition states after the
// store or the slots changed; a new revision.
void publish() {
  shown = {};
  if (demo_mode) {
    shown.add(view::Source{demo_competition, cfg::Provider::kOpenLigaDb, 0});
  } else {
    for (const auto& entry : routes.entries) shown.add(entry);
  }
  error_routes = 0;
  states = {};
  for (uint8_t i = 0; i < route_count; ++i) {
    const auto& slot = slots[i];
    auto& state = states[i];
    state.id = slot.id;
    state.active = slot.active;
    state.fetched_ms = slot.fetched_ms;
    if (!slot.active) continue;
    if (slot.failed) error_routes |= 1u << i;
    state.cause = slot.failed         ? slot.cause
                  : slot.unmaintained ? Cause::kUnmaintained
                                      : Cause::kNone;
    state.provider = slot.provider;
    state.http = slot.failed ? slot.http : 0;
  }
  ++revision;
}
// Under the mutex: oldest last success over the active competitions
// (0 = none yet).
int64_t oldestFetch() {
  int64_t oldest = 0;
  for (uint8_t i = 0; i < route_count; ++i)
    if (slots[i].active && slots[i].fetched_ms != 0 &&
        (oldest == 0 || slots[i].fetched_ms < oldest))
      oldest = slots[i].fetched_ms;
  return oldest;
}
// Under the mutex: marks the competitions of 'out' whose last request
// failed, or with a shown match in its live window while the competition's
// last successful match request is older than kDataStaleS; true if any.
// 'utc' 0 = no clock.
bool markStale(CompetitionStates& out, int64_t utc, int64_t now_ms) {
  bool any = false;
  for (uint8_t i = 0; i < route_count; ++i) {
    out[i].stale = !demo_mode && ((error_routes >> i) & 1) != 0;
    any |= out[i].stale;
  }
  if (demo_mode || utc == 0) return any;
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    uint32_t start = 0, end = 0;
    if (!shown.shows(m) || !schedule::liveWindow(m, start, end) ||
        utc < start || utc > end)
      continue;
    for (uint8_t r = 0; r < route_count; ++r) {
      const auto& slot = slots[r];
      if (slot.id != m.competition || out[r].stale) continue;
      out[r].stale = slot.matches_ms == 0 ||
                     now_ms - slot.matches_ms > cfg::kDataStaleS * 1000LL;
      any |= out[r].stale;
    }
  }
  return any;
}

// Kept data (docs/DATA_FLOW.md "Kept data"): one competition file per
// active competition and the short-name file. File I/O runs only while
// holding the heavy-operation token, which images::format() needs too.
bool fileWrite(const void* bytes, std::size_t size, void* file) {
  return fwrite(bytes, 1, size, static_cast<FILE*>(file)) == size;
}
bool fileRead(void* bytes, std::size_t size, void* file) {
  return fread(bytes, 1, size, static_cast<FILE*>(file)) == size;
}
// Bytes a file of 'size' takes on the filesystem (whole blocks).
std::size_t blocks(std::size_t size) {
  return (size + cfg::kFsBlockBytes - 1) / cfg::kFsBlockBytes *
         cfg::kFsBlockBytes;
}
void competitionPath(uint8_t id, char* out, std::size_t capacity) {
  snprintf(out, capacity, "%s/%s%u%s", cfg::kFsBasePath,
           cfg::kCompetitionFilePrefix, static_cast<unsigned>(id),
           cfg::kCompetitionFileSuffix);
}
void footballPath(const char* name, char* out, std::size_t capacity) {
  snprintf(out, capacity, "%s/%s", cfg::kFsBasePath, name);
}
// Competition id of a competition file name, 0 for any other name.
uint8_t competitionOf(const char* name) {
  const std::string_view text(name), prefix(cfg::kCompetitionFilePrefix),
      suffix(cfg::kCompetitionFileSuffix);
  if (text.size() <= prefix.size() + suffix.size() ||
      text.substr(0, prefix.size()) != prefix ||
      text.substr(text.size() - suffix.size()) != suffix)
    return 0;
  unsigned id = 0;
  for (const char c : text.substr(prefix.size(), text.size() - prefix.size() -
                                                     suffix.size())) {
    if (c < '0' || c > '9' || id > UINT8_MAX) return 0;
    id = id * 10 + static_cast<unsigned>(c - '0');
  }
  return id <= UINT8_MAX ? static_cast<uint8_t>(id) : 0;
}
bool heavyWithin(uint32_t ms) {
  const int64_t deadline = timekeeping::monotonicMs() + ms;
  while (!network::tryHeavy()) {
    if (timekeeping::monotonicMs() >= deadline) return false;
    vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
  }
  return true;
}
// Token holder: writes 'size' bytes from 'encode' through the temporary
// file onto 'path'; false keeps the previous file. Images come first: a
// write leaves the unused image quota (whole blocks) and kImageReserveBytes
// free.
template <class Encode>
bool writeFile(const char* path, std::size_t size, Encode encode) {
  files::Usage usage{};
  if (files::state() != files::State::kMounted ||
      files::usage(usage) != ESP_OK)
    return false;
  const std::size_t image_bytes = images::usage().bytes;
  const std::size_t unused =
      image_bytes < cfg::kImageTotalBytes
          ? blocks(cfg::kImageTotalBytes - image_bytes)
          : 0;
  if (usage.used_bytes + blocks(size) + unused + cfg::kImageReserveBytes >
      usage.total_bytes)
    return false;
  char temp[files::kPathBufferBytes];
  footballPath(cfg::kFootballTempFile, temp, sizeof(temp));
  FILE* file = fopen(temp, "wb");
  if (file == nullptr) return false;
  bool ok = encode(file) == size;
  ok = fflush(file) == 0 && ok;
  ok = fclose(file) == 0 && ok;
  ok = ok && rename(temp, path) == 0;
  if (!ok) unlink(temp);
  return ok;
}
// Football file bytes (whole blocks) besides slot 'except' (worker or token
// holder).
std::size_t keptBytes(int except) {
  std::size_t total = blocks(names_bytes.load());
  for (uint8_t i = 0; i < route_count; ++i)
    if (i != except && slots[i].on_file) total += blocks(slots[i].file_bytes);
  return total;
}
// Token holder: deletes the competition files 'keep' rejects, one directory
// pass per file (no deletion while the directory is open).
template <class Keep>
void deleteFiles(Keep keep) {
  for (;;) {
    char path[files::kPathBufferBytes] = {};
    DIR* dir = opendir(cfg::kFsBasePath);
    if (dir == nullptr) return;
    while (const dirent* entry = readdir(dir)) {
      const uint8_t id = competitionOf(entry->d_name);
      if (id != 0 && !keep(id)) {
        competitionPath(id, path, sizeof(path));
        break;
      }
    }
    closedir(dir);
    if (path[0] == '\0' || unlink(path) != 0) return;
    ESP_LOGI("provider", "removed %s", path);
  }
}
// Worker: deletes the competition files that hold no active slot's data.
void sweep() {
  if (!sweep_pending || files::state() != files::State::kMounted) return;
  if (!network::tryHeavy()) return;
  deleteFiles([](uint8_t id) {
    const Slot* slot = slotOf(id);
    return slot != nullptr && slot->active && slot->on_file;
  });
  network::releaseHeavy();
  sweep_pending = false;
}
persist::Saved savedOf(uint8_t i, int64_t utc) {
  const auto& slot = slots[i];
  persist::Saved saved;
  saved.competition = slot.id;
  saved.route_key = slot.key;
  saved.saved_utc = utc;
  saved.notices = store.notices(slot.id);
  saved.source.resolved = planner.saved(i, routes.entries[i]);
  saved.source.done = slot.done;
  return saved;
}
// A match of 'competition' is in its live window at 'utc'.
bool liveNow(uint8_t competition, int64_t utc) {
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    uint32_t start = 0, end = 0;
    if (m.competition == competition && schedule::liveWindow(m, start, end) &&
        utc >= start && utc <= end)
      return true;
  }
  return false;
}
// Token holder: writes the short-name file from RAM (an empty table deletes
// it) within the football file budget; false keeps the previous file.
bool storeNames() {
  char path[files::kPathBufferBytes];
  footballPath(cfg::kShortNameFile, path, sizeof(path));
  if (names.count() == 0) {
    if (unlink(path) != 0 && errno != ENOENT) return false;
    names_bytes.store(0);
    return true;
  }
  const auto size = names.encode(nullptr, nullptr);
  lock();
  const auto others = keptBytes(-1) - blocks(names_bytes.load());
  unlock();
  if (others + blocks(size) > cfg::kFootballFilesBytes ||
      !writeFile(path, size, [](FILE* file) {
        return names.encode(fileWrite, file);
      }))
    return false;
  names_bytes.store(size);
  return true;
}
// Worker, idle: writes at most one competition file with new data (every
// kCompetitionFileGapS at most, never during a live window) or the
// short-name file after a wipe. The worker alone writes the store, so it
// reads it without the mutex.
void writeKept(int64_t utc) {
  if (files::state() != files::State::kMounted || ota::trial()) return;
  const int64_t now = timekeeping::monotonicMs();
  if (names_pending) {
    if (!network::tryHeavy()) return;
    const bool ok = storeNames();
    network::releaseHeavy();
    if (!ok) ESP_LOGW("provider", "short names not written");
    names_pending = false;
    return;
  }
  for (uint8_t i = 0; i < route_count; ++i) {
    auto& slot = slots[i];
    if (!slot.active || !slot.dirty ||
        (slot.written_ms != 0 &&
         now - slot.written_ms < cfg::kCompetitionFileGapS * 1000LL) ||
        liveNow(slot.id, utc))
      continue;
    const auto saved = savedOf(i, utc);
    const auto size = persist::encode(store, saved, nullptr, nullptr);
    slot.written_ms = now;
    if (size == 0 || keptBytes(i) + blocks(size) > cfg::kFootballFilesBytes) {
      ESP_LOGW("provider", "competition %u not kept: %u B over the budget",
               static_cast<unsigned>(slot.id), static_cast<unsigned>(size));
      continue;
    }
    if (!network::tryHeavy()) {
      slot.written_ms = 0;
      return;
    }
    char path[files::kPathBufferBytes];
    competitionPath(slot.id, path, sizeof(path));
    const bool ok = writeFile(path, size, [&saved](FILE* file) {
      return persist::encode(store, saved, fileWrite, file);
    });
    network::releaseHeavy();
    if (ok) {
      slot.dirty = false;
      slot.on_file = true;
      slot.file_bytes = static_cast<uint32_t>(size);
      ESP_LOGI("provider", "kept competition %u: %u B",
               static_cast<unsigned>(slot.id), static_cast<unsigned>(size));
    } else {
      ESP_LOGW("provider", "competition %u not written",
               static_cast<unsigned>(slot.id));
    }
    return;
  }
}
// Loaded data starts as old as its file (monotonic ms, never 0 = none).
int64_t loadedMs(int64_t saved_utc, int64_t utc, int64_t now) {
  const int64_t at =
      saved_utc > 0 && utc >= saved_utc ? now - (utc - saved_utc) * 1000 : now;
  return at != 0 ? at : -1;
}
// Worker, at boot after applyRoutes(): loads the short names and the
// competition files of the active competitions with unchanged sources;
// every other competition file goes with the next sweep.
void loadKept() {
  if (files::state() != files::State::kMounted ||
      !heavyWithin(cfg::kOperationDeadlineMs))
    return;
  char path[files::kPathBufferBytes];
  footballPath(cfg::kShortNameFile, path, sizeof(path));
  if (FILE* file = fopen(path, "rb")) {
    lock();
    const bool ok = names.decode(fileRead, file);
    unlock();
    struct stat info{};
    names_bytes.store(ok && stat(path, &info) == 0 ? info.st_size : 0);
    fclose(file);
    if (!ok) ESP_LOGW("provider", "short names unreadable");
  }
  time_t utc = 0;
  const bool valid = timekeeping::utcNow(utc);
  for (uint8_t i = 0; i < route_count; ++i) {
    auto& slot = slots[i];
    if (!slot.active) continue;
    competitionPath(slot.id, path, sizeof(path));
    FILE* file = fopen(path, "rb");
    if (file == nullptr) continue;
    persist::Saved saved;
    bool ok = persist::check(fileRead, file, saved) &&
              saved.competition == slot.id && saved.route_key == slot.key;
    struct stat info{};
    ok = ok && stat(path, &info) == 0;
    if (ok) {
      rewind(file);
      lock();
      ok = persist::load(fileRead, file, store, nowClock());
      if (ok) {
        planner.restore(i, routes.entries[i], saved.source.resolved);
        slot.done = saved.source.done;
        slot.on_file = true;
        slot.file_bytes = static_cast<uint32_t>(info.st_size);
        slot.restored_utc = saved.saved_utc;
        slot.fetched_ms = slot.matches_ms = loadedMs(
            saved.saved_utc, valid ? utc : 0, timekeeping::monotonicMs());
      }
      unlock();
    }
    fclose(file);
    ESP_LOGI("provider", "%s %s", ok ? "loaded" : "dropped", path);
    seed_pending |= ok;
  }
  network::releaseHeavy();
  sweep_pending = true;
  lock();
  store.collect();
  publish();
  unlock();
  events::post(events::Event::kDataUpdated);
}
// Worker, first pass with a valid clock after loadKept(): loaded data types
// count as requested when they last completed, and loaded data gets its age.
void seedKept(int64_t utc) {
  const auto now_s = static_cast<uint32_t>(timekeeping::monotonicMs() / 1000);
  for (uint8_t i = 0; i < route_count; ++i) {
    const auto& slot = slots[i];
    if (!slot.active || !slot.on_file) continue;
    for (std::size_t t = 0; t < persist::kKeptTypes; ++t) {
      const int64_t done = slot.done[t];
      if (done <= 0 || done > utc) continue;
      scheduler.seed(
          i, slot.id, static_cast<cfg::DataType>(t),
          static_cast<uint32_t>(std::min<int64_t>(utc - done, UINT32_MAX)),
          now_s);
    }
  }
  lock();
  for (uint8_t i = 0; i < route_count; ++i)
    if (slots[i].restored_utc != 0)
      slots[i].fetched_ms = slots[i].matches_ms = loadedMs(
          slots[i].restored_utc, utc, timekeeping::monotonicMs());
  unlock();
}
// Worker: after images::format() every competition with data and the short
// names are written again.
void relost() {
  if (!files_lost.exchange(false)) return;
  lock();
  for (uint8_t i = 0; i < route_count; ++i) {
    auto& slot = slots[i];
    slot.dirty = slot.active && slot.fetched_ms != 0;
    slot.on_file = false;
    slot.file_bytes = 0;
    slot.written_ms = 0;
  }
  names_bytes.store(0);
  names_pending = names.count() > 0;
  unlock();
}

// Applies a new data generation: boot and a changed demo mode ('reset')
// drop every stored match and start every job afresh; else only
// competitions whose source changed, that were removed or paused,
// or that use a provider in 'providers' (its key or the ESPN opt-in
// changed) lose their data and jobs, the others keep theirs.
void applyRoutes(const settings::Model& model, uint32_t generation,
                 bool reset, uint8_t providers = 0) {
  lock();
  if (reset) {
    store.clear();
    planner.reset();
    for (uint8_t i = 0; i < cfg::kCompetitionMax; ++i) scheduler.forget(i);
    slots = {};
  }
  std::array<Slot, cfg::kCompetitionMax> next{};
  const uint8_t count = static_cast<uint8_t>(routes.entries.size());
  for (uint8_t i = 0; i < count; ++i) {
    const auto& entry = routes.entries[i];
    const uint32_t key = routeKey(entry);
    const Slot* prior = slotOf(entry.id);
    const bool kept = entry.id != 0 && entry.active && prior != nullptr &&
                      prior->active && prior->key == key &&
                      !usesAny(entry, providers);
    if (kept) next[i] = *prior;
    next[i].id = entry.id;
    next[i].active = entry.active && entry.id != 0;
    next[i].key = key;
    if (!kept && entry.id != 0) store.removeCompetition(entry.id);
    // The same competition in the same slot with another source: its jobs
    // start afresh (a changed id in a slot does that by itself).
    if (!kept && slots[i].id == entry.id) scheduler.forget(i);
    // Season, round and failure state start afresh with the jobs.
    if (!kept) planner.forget(i);
  }
  for (uint8_t i = 0; i < route_count; ++i) {
    bool still = false;
    for (uint8_t j = 0; j < count; ++j) still |= next[j].id == slots[i].id;
    if (!still && slots[i].id != 0) store.removeCompetition(slots[i].id);
  }
  slots = next;
  route_count = count;
  demo_mode = model.demo;
  demo_competition = 0;
  configured = model.demo;
  for (uint8_t i = 0; i < count; ++i)
    if (slots[i].active) {
      configured = true;
      if (demo_competition == 0) demo_competition = slots[i].id;
    }
  if (model.demo) {
    if (demo_competition == 0) demo_competition = 1;
    view::demo(store, demo_competition);
    const auto now = timekeeping::monotonicMs();
    for (auto& slot : slots) slot.fetched_ms = slot.matches_ms = now;
  }
  store.collect();
  published_generation = generation;
  publish();
  unlock();
  // Files of removed, paused or changed competitions go (kept slots keep
  // 'on_file'; a reset clears it for every slot).
  sweep_pending = true;
  events::post(events::Event::kDataUpdated);
}
// Every kStoreMaintenanceS: estimated phases and minutes follow the clock,
// passed matchdays go (OQ-202), unused directory entries are freed.
void maintain(int64_t utc) {
  const int32_t offset = timekeeping::localOffset(utc);
  lock();
  store.estimateAll(utc);
  for (uint8_t i = 0; i < route_count; ++i)
    if (slots[i].active) store.expire(slots[i].id, utc, offset);
  store.collect();
  publish();
  unlock();
  events::post(events::Event::kDataUpdated);
}

// How get() ended, for Planner::complete(): refusals before the send keep
// apart a busy token (wait, keep the run), a used daily budget and changed
// settings or network (wait, start the run again) and any other refusal
// (a failure).
plan::Fetch fetchOf(esp_err_t err) {
  if (err == ESP_OK) return plan::Fetch::kOk;
  if (interruption(err)) return plan::Fetch::kInterrupted;
  if (!sent) {
    if (failure_code == err && failure_cause == Cause::kBusy)
      return plan::Fetch::kBusy;
    if (failure_code == err && failure_cause == Cause::kBudget)
      return plan::Fetch::kCapped;
    return plan::Fetch::kRefused;
  }
  return failure_cause == Cause::kRateLimited ? plan::Fetch::kRateLimited
                                              : plan::Fetch::kFailed;
}

// Runs one request of 'job' and reports its outcome to the scheduler.
void runJob(const schedule::Job& job, const settings::Model& model,
            uint32_t generation, uint32_t epoch) {
  const auto& entry = routes.entries[job.slot];
  lock();
  const bool has = planner.request(job, entry, store, nowClock(), request);
  unlock();
  if (!has) {
    scheduler.finished(
        job, schedule::Outcome::kNothing,
        static_cast<uint32_t>(timekeeping::monotonicMs() / 1000));
    return;
  }
  busy.store(true);
  auto err =
      get(request.url, request.array, request.optional, job.provider, model,
          generation, epoch, consumeLocked, nullptr, nullptr, request.body);
  busy.store(false);
  const auto clock = nowClock();
  const auto fetch = fetchOf(err);
  const int32_t offset =
      clock.utc > 0 ? timekeeping::localOffset(clock.utc) : 0;
  lock();
  const auto done = planner.complete(fetch, clock);
  if (done.invalid) {
    ESP_LOGW("provider", "no current season");
    err = stop(ESP_ERR_INVALID_RESPONSE, Cause::kNoSeason);
  }
  const auto outcome = done.outcome;
  const bool more = done.more, ok = outcome == schedule::Outcome::kOk;
  auto& slot = slots[job.slot];
  const bool matches = job.type == cfg::DataType::kFixtures ||
                       job.type == cfg::DataType::kLive;
  if (ok) {
    slot.fetched_ms = timekeeping::monotonicMs();
    if (matches) slot.matches_ms = slot.fetched_ms;
    slot.failed = false;
    slot.dirty = true;
    slot.restored_utc = 0;
    const auto type = static_cast<std::size_t>(job.type);
    if (!more && type < persist::kKeptTypes && clock.utc > 0)
      slot.done[type] = clock.utc;
    // Asked before expire() drops the passed matchday that holds such a
    // match.
    if (matches && clock.utc > 0) {
      slot.unmaintained = store.unmaintained(slot.id, clock.utc);
      slot.provider = job.provider;
    }
    // A provider still serving a passed matchday (an old OpenLigaDB current
    // round) re-inserts it: drop it now, not at the next maintenance.
    if (matches && clock.utc > 0) store.expire(slot.id, clock.utc, offset);
  } else if (outcome != schedule::Outcome::kDeferred ||
             fetch == plan::Fetch::kCapped) {
    // A busy token or changed settings is no failure of the source; a used
    // daily budget stays visible on the route.
    slot.failed = true;
    slot.cause = causeOf(err);
    slot.provider = job.provider;
    slot.http = httpOf(err);
  }
  publish();
  unlock();
  // A sent request counts from its start, a refusal from now.
  scheduler.finished(
      job, outcome,
      sent ? sent_s : static_cast<uint32_t>(timekeeping::monotonicMs() / 1000),
      more);
  events::post(events::Event::kDataUpdated);
}

struct SelectContext {
  const SelectionRequest* request;
  unsigned seen = 0;
  // The page is full and one more match is known: the rest is not read.
  bool enough = false;
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
    selection_state.more = c.enough = true;
  else
    selections[selection_state.count++] = s;
  unlock();
  return true;
}
void finishSelection(esp_err_t err) {
  lock();
  if (err != ESP_OK) {
    // Distinct codes so the Web UI can explain each cause.
    selection_state.cause = causeOf(err);
    selection_state.http = httpOf(err);
    selection_state.count = 0;
  } else {
    selection_state.cause = Cause::kNone;
    selection_state.http = 0;
  }
  selection_state.busy = false;
  selection_state.wait = Cadence::kNone;
  selection_state.wait_ms = 0;
  ++selection_state.revision;
  // Cleared under the mutex: select() checks it there before a new job.
  selecting.store(false);
  unlock();
}
// The owner's selection as a resumable work item (worker only, OQ-261): one
// request per worker pass, admitted by the scheduler like a job. While the
// provider's window limits or a pause hold it, the worker serves the other
// jobs and the Web UI shows until when the selection waits; a busy heavy
// token retries after the device gap, for at most kOperationDeadlineMs.
// Stages: the provider's current season (a club list at OpenLigaDB and
// API-Football needs it in the URL), the list, and one more list attempt
// after a brief failure.
struct SelectionRun {
  enum Stage : uint8_t { kSeason, kList, kRetry };
  bool active = false;
  Stage stage = kList;
  uint32_t generation = 0, epoch = 0;
  // Monotonic ms the heavy token first refused this stage (0 = not busy).
  int64_t busy_since_ms = 0;
  // The season in the club list's URL (the owner's, or the current one).
  char season[33]{};
};
SelectionRun run;
void endSelection(esp_err_t err) {
  run = {};
  finishSelection(err);
}
// Season consumers: each OpenLigaDB match row carries leagueSeason; an
// API-Football league row marks its current season.
bool seasonRow(ArduinoJson::JsonVariantConst row, void* context) {
  auto* r = static_cast<SelectionRun*>(context);
  if (!row["leagueSeason"].is<int>()) return false;
  snprintf(r->season, sizeof(r->season), "%d", row["leagueSeason"].as<int>());
  return true;
}
bool apiSeasonRow(ArduinoJson::JsonVariantConst row, void* context) {
  auto* r = static_cast<SelectionRun*>(context);
  for (auto item : row["seasons"].as<ArduinoJson::JsonArrayConst>())
    if (item["current"].as<bool>() && item["year"].is<int>()) {
      snprintf(r->season, sizeof(r->season), "%d", item["year"].as<int>());
      return true;
    }
  return true;
}
// The URL and array key of the current stage of 'request'.
const char* selectionUrl(const SelectionRequest& request, int64_t utc,
                         char* url, std::size_t capacity) {
  char comp[3 * cfg::kCompetitionIdMaxBytes + 1];
  char country[3 * cfg::kSelectionTextMaxBytes + 1];
  plan::encode(request.competition.data, comp, sizeof(comp));
  const char* season = run.season;
  const auto p = request.provider;
  if (run.stage == SelectionRun::kSeason) {
    if (p == cfg::Provider::kOpenLigaDb) {
      // The current round's matches carry leagueSeason (about 18 KB for bl1).
      snprintf(url, capacity, "https://api.openligadb.de/getmatchdata/%s",
               comp);
      return "";
    }
    snprintf(url, capacity,
             "https://v3.football.api-sports.io/leagues?id=%s&current=true",
             comp);
    return "response";
  }
  if (p == cfg::Provider::kOpenLigaDb) {
    if (request.teams) {
      snprintf(url, capacity,
               "https://api.openligadb.de/getavailableteams/%s/%s", comp,
               season);
      return "";
    }
    char year[12];
    if (request.season.empty()) {
      const time_t stamp = utc;
      std::tm date{};
      gmtime_r(&stamp, &date);
      snprintf(year, sizeof(year), "%d",
               date.tm_year + 1900 -
                   (date.tm_mon + 1 < cfg::kSeasonStartMonth ? 1 : 0));
      season = year;
    }
    snprintf(url, capacity, "https://api.openligadb.de/getavailableleagues/%s",
             season);
    return "";
  }
  if (p == cfg::Provider::kApiFootball) {
    if (request.teams) {
      snprintf(url, capacity,
               "https://v3.football.api-sports.io/teams?league=%s&season=%s",
               comp, season);
    } else {
      plan::encode(request.country.empty() ? cfg::kSelectionCountryDefault
                                           : request.country.data,
                   country, sizeof(country));
      snprintf(url, capacity,
               "https://v3.football.api-sports.io/leagues?country=%s", country);
    }
    return "response";
  }
  if (p == cfg::Provider::kFootballData) {
    if (request.teams)
      snprintf(url, capacity,
               "https://api.football-data.org/v4/competitions/%s/teams%s%s",
               comp, request.season.empty() ? "" : "?season=", season);
    else
      snprintf(url, capacity, "https://api.football-data.org/v4/competitions");
    return request.teams ? "teams" : "competitions";
  }
  if (request.teams)
    snprintf(url, capacity,
             "https://site.api.espn.com/apis/site/v2/sports/soccer/%s/"
             "teams?limit=1000%s%s",
             comp, request.season.empty() ? "" : "&season=", season);
  else
    snprintf(url, capacity,
             "https://site.api.espn.com/apis/site/v2/leagues/"
             "dropdown?sport=soccer&limit=1000");
  return request.teams ? "teams" : "leagues";
}
void setSelectionWait(Cadence wait, int64_t wait_ms) {
  lock();
  selection_state.wait = wait;
  selection_state.wait_ms = wait_ms;
  unlock();
}
// One step of the owner's selection. Returns true when it ran a request (the
// worker then decides again); otherwise 'wake_ms' is the monotonic ms it may
// go on.
bool stepSelection(const settings::Model& model, uint32_t generation,
                   int64_t utc, int64_t& wake_ms) {
  SelectionRequest request;
  lock();
  request = selection_request;
  unlock();
  if (!run.active) {
    run = {};
    run.active = true;
    run.generation = generation;
    run.epoch = network::status().epoch;
    memcpy(run.season, request.season.data, sizeof(run.season));
    run.stage = request.teams && request.season.empty() &&
                        (request.provider == cfg::Provider::kOpenLigaDb ||
                         request.provider == cfg::Provider::kApiFootball)
                    ? SelectionRun::kSeason
                    : SelectionRun::kList;
  }
  const int64_t now_ms = timekeeping::monotonicMs();
  const auto now_s = static_cast<uint32_t>(now_ms / 1000);
  schedule::Wait why = schedule::Wait::kIdle;
  const auto at = scheduler.admissibleAt(request.provider, now_s, &why);
  if (at > now_s) {
    // The provider's window limit or pause holds the request; the gap and
    // a busy device are short and shown as loading.
    const auto shown = why == schedule::Wait::kWindow   ? Cadence::kLimit
                       : why == schedule::Wait::kPaused ? Cadence::kRetry
                                                        : Cadence::kNone;
    wake_ms = at * 1000LL;
    setSelectionWait(shown, shown == Cadence::kNone ? 0 : wake_ms);
    return false;
  }
  setSelectionWait(Cadence::kNone, 0);
  char url[384];
  const char* key = selectionUrl(request, utc, url, sizeof(url));
  esp_err_t err;
  busy.store(true);
  if (run.stage == SelectionRun::kSeason) {
    run.season[0] = '\0';
    err =
        get(url, key, false, request.provider, model, run.generation, run.epoch,
            request.provider == cfg::Provider::kOpenLigaDb ? seasonRow
                                                           : apiSeasonRow,
            &run, nullptr, cfg::kSelectionBodyBytes);
    if (err == ESP_OK && run.season[0] == '\0') err = ESP_ERR_INVALID_RESPONSE;
  } else {
    SelectContext context{&request};
    err =
        get(url, key, false, request.provider, model, run.generation, run.epoch,
            selectionRow, &context, &context.enough, cfg::kSelectionBodyBytes);
  }
  busy.store(false);
  if (fetchOf(err) == plan::Fetch::kBusy) {
    if (run.busy_since_ms == 0) run.busy_since_ms = now_ms;
    if (now_ms - run.busy_since_ms < cfg::kOperationDeadlineMs) {
      wake_ms = now_ms + cfg::kRequestGapMs;
      return false;
    }
    endSelection(err);
    return true;
  }
  run.busy_since_ms = 0;
  if (err == ESP_OK && run.stage == SelectionRun::kSeason) {
    run.stage = SelectionRun::kList;
    return true;
  }
  // A server error (5xx) or a failed connection is often brief: one more
  // list attempt, admitted like any request, with the rows of the first one
  // dropped.
  const bool brief = failure_cause == Cause::kConnect ||
                     (failure_cause == Cause::kHttp && failure_status >= 500 &&
                      failure_status <= 599);
  if (err != ESP_OK && brief && run.stage == SelectionRun::kList) {
    run.stage = SelectionRun::kRetry;
    lock();
    selection_state.count = 0;
    selection_state.more = false;
    unlock();
    return true;
  }
  endSelection(err);
  return true;
}

// Why nothing runs now (Web UI).
Cadence cadenceOf(const schedule::Decision& d, int64_t utc, uint32_t now_s) {
  using schedule::Wait;
  if (d.wait == Wait::kDaily) return Cadence::kBudgetUsed;
  if (d.wait == Wait::kPaused) return Cadence::kRetry;
  if (d.wait == Wait::kWindow) return Cadence::kLimit;
  if (error_routes) return Cadence::kRetry;
  if (scheduler.degraded()) return Cadence::kBudget;
  bool live = false, prematch = false, postmatch = false, opens = false;
  const int64_t wake_utc =
      d.wake_s ? utc + (static_cast<int64_t>(d.wake_s) - now_s) : INT64_MAX;
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    uint32_t start = 0, end = 0;
    if (!shown.shows(m) || !schedule::liveWindow(m, start, end) || utc > end)
      continue;
    if (utc < start) {
      // The scheduler wakes for this live window.
      opens |= start <= wake_utc;
      continue;
    }
    if (m.phase == Phase::kFinished)
      postmatch = true;
    else if (view::started(m.phase))
      live = true;
    else
      prematch = true;
  }
  return live        ? Cadence::kLive
         : prematch  ? Cadence::kPrematch
         : postmatch ? Cadence::kPostmatch
         : opens     ? Cadence::kWindow
                     : Cadence::kIdle;
}
// Replaces the worker's settings and competition copies. Out of line, so the
// temporary copy lives in this frame only and not in the worker's frame
// during requests.
[[gnu::noinline]] void reload(settings::Model& model) {
  model = settings::current();
  settings::competitions(routes);
}
void setCadence(Cadence cadence, int64_t due) {
  lock();
  cadence_now = cadence;
  due_ms = due;
  unlock();
}
void worker(void*) {
  uint32_t settings_seen = settings::generation();
  settings::Model model;
  reload(model);
  uint32_t applied = settings::dataGeneration();
  Access access = accessOf(model);
  applyRoutes(model, applied, true);
  if (!model.demo) loadKept();
  int64_t maintained_ms = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
    relost();
    // Read before the copy: a save in between then interrupts the request.
    const auto generation = settings::dataGeneration();
    if (const auto seen = settings::generation();
        seen != settings_seen || generation != applied) {
      settings_seen = seen;
      const bool demo = model.demo;
      const Access prior_access = access;
      reload(model);
      access = accessOf(model);
      if (generation != applied) {
        applyRoutes(model, generation, model.demo != demo,
                    changedProviders(prior_access, access));
        applied = generation;
      }
      // Changed request plans and limits apply with the next decision.
    }
    // A clock step re-estimates at once. Neither a clock step nor a new
    // network epoch fetches again: overdue jobs run anyway.
    if (time_changed.exchange(false)) maintained_ms = 0;
    time_t utc = 0;
    const bool clock_valid = timekeeping::utcNow(utc);
    if (ota::trial()) {
      setCadence(Cadence::kUpdateTest, 0);
      continue;
    }
    if ((!network::ready() || !clock_valid) && !model.demo) {
      // Under the mutex, like select(), so a new request cannot be lost.
      lock();
      cadence_now = Cadence::kWaiting;
      due_ms = 0;
      if (selecting.load()) {
        run = {};
        selection_state.wait = Cadence::kNone;
        selection_state.wait_ms = 0;
        selection_state.busy = false;
        selection_state.count = 0;
        selection_state.cause =
            network::ready() ? Cause::kTime : Cause::kNetwork;
        selection_state.http = 0;
        ++selection_state.revision;
        selecting.store(false);
      }
      unlock();
      continue;
    }
    day = static_cast<uint32_t>(utc / 86400);
    // A counter that could not be read counts as used up on this day only
    // (the demo runs without a valid clock: no day to settle).
    if (clock_valid) budget::settle(day);
    const int64_t now = timekeeping::monotonicMs();
    if (!model.demo && (maintained_ms == 0 ||
                        now - maintained_ms >= cfg::kStoreMaintenanceS * 1000LL)) {
      maintained_ms = now;
      maintain(utc);
    }
    // The owner's selection goes first; while it waits for its provider,
    // the jobs run.
    int64_t selection_wake = INT64_MAX;
    if (selecting.load() &&
        stepSelection(model, generation, utc, selection_wake))
      continue;
    if (model.demo) {
      setCadence(Cadence::kIdle, 0);
      continue;
    }
    if (seed_pending) {
      seed_pending = false;
      seedKept(utc);
    }
    if (requested.exchange(false)) scheduler.refresh();
    for (uint8_t i = 0; i < cfg::kCompetitionMax; ++i)
      planner.wanted(i,
                     i < routes.entries.size() ? &routes.entries[i] : nullptr,
                     store, nowClock(), inputs.competitions[i]);
    inputs.plans = &model.providers;
    inputs.global = model.global_limit;
    for (unsigned p = 0; p < cfg::kProviderCount; ++p) {
      const auto daily = model.providers[p].limit.daily;
      inputs.daily_left[p] =
          daily ? budget::remaining(static_cast<cfg::Provider>(p), day, daily)
                : schedule::kUnlimited;
    }
    inputs.global_daily_left =
        model.global_limit.daily
            ? budget::remainingGlobal(day, model.global_limit.daily)
            : schedule::kUnlimited;
    inputs.store = &store;
    inputs.clock = nowClock();
    const auto decision = scheduler.next(inputs);
    if (decision.run) {
      runJob(decision.job, model, generation, network::status().epoch);
      continue;
    }
    sweep();
    writeKept(utc);
    const int64_t due = decision.wake_s ? decision.wake_s * 1000LL : 0;
    const auto cadence = cadenceOf(decision, utc, inputs.clock.mono_s);
    lock();
    cadence_now = cadence;
    due_ms = due;
    retry_ms = error_routes ? due : 0;
    unlock();
    // Sleeps until the wake time (at most until the next maintenance) or
    // until something asks for a new decision.
    const int64_t wake = std::min({due ? due : INT64_MAX, selection_wake,
                                   now + cfg::kStoreMaintenanceS * 1000LL});
    const uint32_t epoch = network::status().epoch;
    while (timekeeping::monotonicMs() < wake && !requested.load() &&
           (!selecting.load() || run.active) && !time_changed.load() &&
           settings::generation() == settings_seen &&
           settings::dataGeneration() == generation && network::ready() &&
           network::status().epoch == epoch && !ota::trial())
      vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
  }
}
}  // namespace
esp_err_t init() {
  mutex = xSemaphoreCreateMutexStatic(&mutex_buffer);
  const auto budget_error = budget::init();
  if (budget_error != ESP_OK)
    ESP_LOGE("provider", "request counters unreadable: %s",
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
  if (mutex == nullptr) return s;
  s.started = true;
  time_t utc = 0;
  const bool valid = timekeeping::utcNow(utc);
  const int64_t now = timekeeping::monotonicMs();
  lock();
  s.revision = revision;
  s.generation = published_generation;
  memcpy(s.requests, requests, sizeof(requests));
  s.fetched_ms = oldestFetch();
  s.matches = static_cast<uint8_t>(
      std::min<std::size_t>(UINT8_MAX, store.matchCount()));
  s.competitions = states;
  s.stale = markStale(s.competitions, valid ? utc : 0, now);
  for (uint8_t i = 0; i < route_count; ++i)
    s.matches_truncated |=
        slots[i].active && (store.notices(slots[i].id) & kNoticeMatches) != 0;
  s.cadence = cadence_now;
  const bool scheduled = cadence_now != Cadence::kNone &&
                         cadence_now != Cadence::kWaiting &&
                         cadence_now != Cadence::kUpdateTest;
  s.next_ms = scheduled ? due_ms : 0;
  unlock();
  s.busy = busy.load();
  return s;
}
const char* cadenceName(Cadence cadence) {
  switch (cadence) {
    case Cadence::kNone: return "none";
    case Cadence::kLive: return "live";
    case Cadence::kPrematch: return "prematch";
    case Cadence::kPostmatch: return "postmatch";
    case Cadence::kIdle: return "idle";
    case Cadence::kWindow: return "window";
    case Cadence::kBudget: return "budget";
    case Cadence::kBudgetUsed: return "budget_used";
    case Cadence::kLimit: return "limit";
    case Cadence::kRetry: return "retry";
    case Cadence::kWaiting: return "waiting";
    case Cadence::kUpdateTest: return "update_test";
  }
  return "none";
}
void read(Reader reader, void* context) {
  if (mutex == nullptr) return;
  time_t utc = 0;
  const bool valid = timekeeping::utcNow(utc);
  const int64_t now = timekeeping::monotonicMs();
  lock();
  const bool stale = markStale(states, valid ? utc : 0, now);
  const Published data{
      store,      shown, names,         revision, published_generation,
      configured, stale, oldestFetch(), retry_ms, states};
  reader(data, context);
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
    selection_state.count = 0;
    selection_state.more = false;
    selection_state.cause = Cause::kNone;
    selection_state.provider = request.provider;
    selection_state.http = 0;
    selecting.store(true);
  }
  unlock();
  return running ? ESP_ERR_NOT_FINISHED : ESP_OK;
}
}  // namespace
esp_err_t select(const SelectionRequest& request) { return start(request); }
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
  for (uint8_t i = 0; i < route_count; ++i) {
    const auto& slot = slots[i];
    if (!slot.active) continue;
    auto& info = result.routes[i];
    info.matches = static_cast<uint8_t>(std::min<uint16_t>(
        UINT8_MAX, view::matchCount(store, shown, slot.id)));
    if (const auto* source = shown.source(slot.id))
      info.provider = source->provider;
  }
  const int best = view::ownForOverview(store, shown, utc);
  if (best >= 0) {
    const auto& m = store.match(best);
    auto& own = result.own;
    own.present = true;
    for (uint8_t i = 0; i < route_count; ++i)
      if (slots[i].id == m.competition) own.route = i;
    own.phase = m.phase;
    own.kickoff = m.kickoff;
    if (m.principal.valid()) {
      own.home_goals = m.principal.home;
      own.away_goals = m.principal.away;
    }
    if (m.minute != kNoValue) own.minute = m.minute;
    own.home.assign(view::fullName(store, m, true));
    own.away.assign(view::fullName(store, m, false));
  }
  unlock();
  return result;
}
namespace {
// Under the mutex: store team indices the matches and rows of
// 'competition' refer to.
std::bitset<cfg::kStoreTeams> teamsOf(uint8_t competition) {
  std::bitset<cfg::kStoreTeams> used;
  const auto mark = [&used](uint8_t team) {
    if (team < cfg::kStoreTeams && store.teamAt(team).id != 0) used.set(team);
  };
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (m.competition != competition) continue;
    mark(m.home);
    mark(m.away);
  }
  for (std::size_t i = 0; i < store.rowCount(); ++i)
    if (store.row(i).competition == competition) mark(store.row(i).team);
  return used;
}
}  // namespace
uint16_t teamCount(uint8_t competition, uint32_t& revision_out) {
  revision_out = 0;
  if (mutex == nullptr) return 0;
  lock();
  revision_out = revision;
  const auto count = static_cast<uint16_t>(teamsOf(competition).count());
  unlock();
  return count;
}
bool teamAt(uint8_t competition, uint16_t index, uint32_t revision_in,
            TeamInfo& out) {
  if (mutex == nullptr) return false;
  bool found = false;
  lock();
  if (revision_in == revision) {
    const auto used = teamsOf(competition);
    for (std::size_t i = 0, seen = 0; i < used.size() && !found; ++i) {
      if (!used.test(i) || seen++ != index) continue;
      const auto& team = store.teamAt(static_cast<uint8_t>(i));
      out.id = team.id;
      out.provider = team.provider;
      out.short_name = team.short_name;
      const char* custom = names.find(team.provider, team.id);
      out.custom.assign(custom ? custom : "");
      found = true;
    }
  }
  unlock();
  return found;
}
esp_err_t setShortName(cfg::Provider provider, uint32_t id,
                       std::string_view name) {
  if (mutex == nullptr || files::state() != files::State::kMounted ||
      ota::trial())
    return ESP_ERR_INVALID_STATE;
  lock();
  const char* prior_text = names.find(provider, id);
  Text<26> prior;
  prior.assign(prior_text ? prior_text : "");
  const bool set = names.set(provider, id, name);
  unlock();
  if (!set) return ESP_ERR_NO_MEM;
  const bool ok = storeNames();
  lock();
  if (!ok) names.set(provider, id, prior.data);
  ++revision;
  unlock();
  if (!ok) return ESP_ERR_NO_MEM;
  events::post(events::Event::kDataUpdated);
  return ESP_OK;
}
esp_err_t replaceShortNames(persist::ShortNames& table) {
  if (mutex == nullptr || files::state() != files::State::kMounted ||
      ota::trial())
    return ESP_ERR_INVALID_STATE;
  lock();
  names.swap(table);
  unlock();
  const bool ok = storeNames();
  lock();
  if (!ok) names.swap(table);
  ++revision;
  unlock();
  if (!ok) return ESP_ERR_NO_MEM;
  events::post(events::Event::kDataUpdated);
  return ESP_OK;
}
void shortNames(persist::ShortNames& out) {
  if (mutex == nullptr) return out.clear();
  lock();
  out = names;
  unlock();
}
void filesLost() { files_lost.store(true); }
void eraseFiles() {
  if (files::state() != files::State::kMounted) return;
  deleteFiles([](uint8_t) { return false; });
  char path[files::kPathBufferBytes];
  footballPath(cfg::kShortNameFile, path, sizeof(path));
  unlink(path);
  footballPath(cfg::kFootballTempFile, path, sizeof(path));
  unlink(path);
}
}  // namespace football
