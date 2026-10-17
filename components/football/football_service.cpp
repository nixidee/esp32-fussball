#include "football_service.h"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

#include "dirent.h"
#include "esp_log.h"
#include "event_bus.h"
#include "file_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "image_service.h"
#include "network_service.h"
#include "nvs.h"
#include "ota_service.h"
#include "provider_client.h"
#include "provider_coverage.h"
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
settings::screen_policy::Eligibility eligibility;
coverage::EvidenceArray coverage_evidence;
coverage::Discovery discovery;
std::atomic<uint32_t> coverage_bytes{0};
bool coverage_dirty = false, coverage_streaming = false;
bool discovery_retire = false;
int64_t coverage_write_ms = 0;
coverage::Progress discovery_progress;
uint64_t coverage_binding = 0;

// The caller already owns the heavy token. All durable guards complete before
// the first HTTP write, not during DNS/TLS. Implemented with the file helpers
// below; no file I/O is performed under the repository read mutex.
bool storeCoverage(uint8_t pending_competition = 0);
bool fenceCoverage(uint8_t competition);
bool reserveDiscovery(uint32_t utc_day);
coverage::Evidence* coverageOf(uint8_t id);

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
  bool matches_loaded = false, table_loaded = false;
  uint32_t file_bytes = 0;
  int64_t written_ms = 0, restored_utc = 0;
};
std::array<Slot, cfg::kCompetitionMax> slots{};
uint8_t route_count = 0;
bool configured = false, demo_mode = false;
demo::Scenario demo_scenario = demo::Scenario::kOwn;
uint8_t demo_step = 0;
Clock demo_clock;
demo::Bindings demo_bindings;
constexpr uint16_t kNoDemoCommand = UINT16_MAX;
std::atomic<uint16_t> demo_command{kNoDemoCommand};
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
DateContext localDates(int64_t utc) {
  return {[](int64_t at, void*) { return timekeeping::localOffset(at); },
          nullptr, utc > 0 ? timekeeping::localOffset(utc) : 0};
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
// What the stored data of a competition depends on: its service-owned
// identity and source. A reused small id must not retain an old slot/cache.
uint32_t routeKey(const settings::Competition& e) {
  return sourceHash(fnv(fnv(2166136261u, e.id), e.incarnation), e.primary);
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
uint64_t coverageBinding(const settings::Model& model) {
  uint32_t low = 2166136261u, high = 0x9e3779b9u;
  for (const auto& entry : routes.entries) {
    for (const auto word : {routeKey(entry), uint32_t{entry.active}}) {
      low = fnv(low, word);
      high = fnv(high, word);
    }
  }
  for (const auto word : accessOf(model)) {
    low = fnv(low, word);
    high = fnv(high, word);
  }
  return (uint64_t{high} << 32) | low;
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
// Error class of an esp_err_t for the pure cause mapping (football_model.h).
FetchError fetchErrorOf(esp_err_t err) {
  switch (err) {
    case ESP_ERR_NO_MEM: return FetchError::kNoMemory;
    case ESP_ERR_INVALID_SIZE: return FetchError::kTooLarge;
    case ESP_ERR_INVALID_RESPONSE: return FetchError::kBadResponse;
    default: return FetchError::kOther;
  }
}
// The cause recorded for this error, else one derived from the plain error
// code (failures outside get()).
Cause causeOf(esp_err_t err) {
  if (failure_cause != Cause::kNone && failure_code == err)
    return failure_cause;
  return errorCause(fetchErrorOf(err));
}
// The HTTP error status behind 'err' (0 = none: refused before sending, no
// response, or a 2xx response that could not be used).
uint16_t httpOf(esp_err_t err) {
  if (!sent || failure_code != err) return 0;
  return errorStatus(failure_status);
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
// The last get() stopped because settings, network or the valid clock changed.
bool interruption(esp_err_t err) {
  return failure_code == err &&
         (failure_cause == Cause::kSettings ||
          failure_cause == Cause::kNetwork || failure_cause == Cause::kTime);
}
enum class HttpPurpose : uint8_t { kAutomatic, kSetup, kDiscovery };

// The first actual HTTP send attempt owns the durable reservation and the
// scheduler/counter charge. DNS/TLS alone reserve nothing. The client calls
// this exactly once, immediately before its first nonblocking HTTP write.
uint32_t sent_s = 0;
struct SendContext {
  cfg::Provider provider;
  const settings::Model& model;
  uint32_t generation, settings_generation, epoch;
  HttpPurpose purpose;
  uint8_t competition;
};
esp_err_t beginSend(void* context) {
  const auto& send = *static_cast<SendContext*>(context);
  const auto admitted =
      settings::beginFootballSend(send.purpose == HttpPurpose::kSetup,
                                  send.generation, send.settings_generation);
  if (admitted != ESP_OK)
    return stop(admitted, admitted == ESP_ERR_INVALID_STATE ? Cause::kSettings
                                                            : Cause::kBusy);
  // Success holds the existing settings writer mutex. Every refusal releases
  // it here; a successful admission transfers it to endSend after one write.
  const auto refuse = [](esp_err_t err, Cause cause) {
    settings::endFootballSend();
    return stop(err, cause);
  };
  if (const auto stopped = interrupted(send.generation, send.epoch))
    return refuse(ESP_ERR_INVALID_STATE, stopped.cause);
  time_t utc = 0;
  if (!timekeeping::utcNow(utc))
    return refuse(ESP_ERR_INVALID_STATE, Cause::kTime);
  // DNS/TLS may cross UTC midnight. Charge the send's actual day, not the
  // day on which the worker began resolving the request.
  const auto send_day = static_cast<uint32_t>(utc / 86400);
  if (send_day != day) {
    day = send_day;
    budget::settle(day);
  }
  const auto index = static_cast<unsigned>(send.provider);
  const auto& global = send.model.global_limit;
  if (global.daily && budget::remainingGlobal(day, global.daily) == 0)
    return refuse(ESP_ERR_NOT_ALLOWED, Cause::kBudget);
  if (send.purpose == HttpPurpose::kDiscovery && !reserveDiscovery(send_day))
    return refuse(ESP_ERR_NOT_ALLOWED, Cause::kCounter);
  if (send.competition != 0 && !fenceCoverage(send.competition))
    return refuse(ESP_ERR_INVALID_STATE, Cause::kCounter);
  auto err = budget::admit(send.provider, day,
                           send.model.providers[index].limit.daily);
  if (err == ESP_OK) err = budget::admitGlobal(day, global.daily);
  if (err != ESP_OK)
    return refuse(
        err, err == ESP_ERR_NOT_ALLOWED ? Cause::kBudget : Cause::kCounter);
  sent_s = static_cast<uint32_t>(timekeeping::monotonicMs() / 1000);
  scheduler.sent(send.provider, sent_s);
  lock();
  ++requests[index];
  unlock();
  sent = true;
  return ESP_OK;
}
void endSend(void*) { settings::endFootballSend(); }

// One provider request the scheduler has admitted (a job, or a step of the
// owner's selection that asked admissibleAt()). It never waits: a busy heavy
// token, a used daily budget or changed settings refuse it before anything is
// sent, and the caller reschedules. The scheduler counts the request when it
// is really sent (sent()); 'sent_s' keeps that second for finished(). 'body'
// caps the response body (plan::Request::body).
esp_err_t get(const char* url, const char* array, bool optional,
              cfg::Provider p, const settings::Model& m, uint32_t generation,
              uint32_t settings_generation, uint32_t epoch,
              JsonConsumer consumer, void* context,
              const bool* enough = nullptr,
              std::size_t body = cfg::kProviderBodyBytes,
              HttpPurpose purpose = HttpPurpose::kAutomatic,
              const JsonObserver* observer = nullptr, JsonScan* scan = nullptr,
              uint8_t pending_competition = 0,
              const network::HeavyGuard* admission = nullptr) {
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
  if (purpose != HttpPurpose::kSetup && settings::footballPaused())
    return skip("picture frame", ESP_ERR_TIMEOUT, Cause::kBusy);
  if (const auto stopped = interrupted(generation, epoch))
    return skip(stopped.why, ESP_ERR_INVALID_STATE, stopped.cause);
  // Another heavy operation (OTA, upload, image work) runs: the request
  // waits for the next pass. Debug level, since it repeats until the token
  // is free.
  const bool owns_admission = admission == nullptr;
  if ((admission && !*admission) || (owns_admission && !network::tryHeavy())) {
    ESP_LOGD("provider", "not sent: %s (operation busy)", url);
    return stop(ESP_ERR_TIMEOUT, Cause::kBusy);
  }
  if (purpose != HttpPurpose::kSetup && settings::footballPaused()) {
    if (owns_admission) network::releaseHeavy();
    return skip("picture frame", ESP_ERR_TIMEOUT, Cause::kBusy);
  }
  SendContext send_context{
      p,     m,       generation,         settings_generation,
      epoch, purpose, pending_competition};
  const SendHooks send_hooks{beginSend, endSend, &send_context,
                             purpose != HttpPurpose::kSetup};
  FetchResult result;
  const int64_t started = timekeeping::monotonicMs();
  const auto err =
      fetchJson(url, array, p, m, generation, consumer, context, body, &result,
                optional, enough, &send_hooks, observer, scan);
  if (owns_admission) network::releaseHeavy();
  const auto elapsed =
      static_cast<long long>(timekeeping::monotonicMs() - started);
  if (result.paused) {
    ESP_LOGD("provider", "not sent: %s (picture frame during DNS/TLS)", url);
    return stop(ESP_ERR_TIMEOUT, Cause::kBusy);
  }
  if (result.admission_error != ESP_OK) {
    ESP_LOGD("provider", "not sent: %s (local admission: %s)", url,
             esp_err_to_name(result.admission_error));
    // beginSend already paired the exact local reason with this code.
    return result.admission_error;
  }
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
      failure_cause = fetchCause(fetchErrorOf(err), result.status);
      failure_code = err;
      failure_status = result.status;
    }
  }
  return err;
}
// fetchJson consumer of the provider plan: each element is mapped into the
// store under the mutex.
bool consumeLocked(ArduinoJson::JsonVariantConst element, void* raw) {
  lock();
  if (raw != nullptr && planner.acceptsMatch(element)) {
    const auto& entry = *static_cast<const settings::Competition*>(raw);
    if (auto* evidence = coverageOf(entry.id))
      coverage_dirty |= coverage::observeNormalRow(*evidence, entry, element);
  }
  const bool ok = plan::Planner::consume(element, &planner);
  unlock();
  return ok;
}

// Under the mutex: the shown sources and the competition states after the
// store or the slots changed; a new revision.
void publish() {
  shown = {};
  for (const auto& entry : routes.entries) shown.add(entry);
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
    state.notices = store.notices(slot.id);
    state.cause = slot.failed         ? slot.cause
                  : slot.unmaintained ? Cause::kUnmaintained
                  : state.notices     ? Cause::kStoreFull
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
  for (const char c : text.substr(
           prefix.size(), text.size() - prefix.size() - suffix.size())) {
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
  if (files::state() != files::State::kMounted || files::usage(usage) != ESP_OK)
    return false;
  const std::size_t image_bytes = images::usage().bytes;
  const std::size_t unused = image_bytes < cfg::kImageTotalBytes
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
  std::size_t total =
      blocks(names_bytes.load()) + blocks(coverage_bytes.load());
  for (uint8_t i = 0; i < route_count; ++i)
    if (i != except && slots[i].on_file) total += blocks(slots[i].file_bytes);
  return total;
}

coverage::Evidence* coverageOf(uint8_t id) {
  for (auto& row : coverage_evidence)
    if (id != 0 && row.id == id) return &row;
  return nullptr;
}
void bindCoverage() {
  for (auto& row : coverage_evidence) {
    const auto* entry = routes.find(row.id);
    if (entry == nullptr || !entry->active) row = {};
  }
  for (const auto& entry : routes.entries) {
    if (!entry.active || entry.id == 0) continue;
    auto* row = coverageOf(entry.id);
    if (row == nullptr)
      for (auto& free : coverage_evidence)
        if (free.id == 0) {
          row = &free;
          break;
        }
    if (row != nullptr && !coverage::matches(*row, coverage::identity(entry)))
      coverage::bind(*row, coverage::identity(entry));
  }
  coverage_dirty = true;
}

// NVS remains the single authority for run/request credits even when image
// formatting removes every football file. The file's encoded guard is only a
// transaction snapshot, never a second source of credits.
void loadDiscovery() {
  std::array<uint8_t, coverage::record::kGuardBytes> bytes{};
  std::size_t size = bytes.size();
  coverage::DiscoveryRecord record;
  budget::Load state = budget::Load::kMissing;
  nvs_handle_t handle;
  const auto opened = nvs_open(cfg::kBudgetNvsNamespace, NVS_READONLY, &handle);
  if (opened == ESP_OK) {
    const auto err =
        nvs_get_blob(handle, cfg::kCoverageGuardNvsKey, bytes.data(), &size);
    state = err == ESP_ERR_NVS_NOT_FOUND ? budget::Load::kMissing
            : err == ESP_OK && coverage::record::decodeGuard(
                                   {bytes.data(), size}, record) ==
                                   coverage::record::DecodeResult::kOk
                ? budget::Load::kValid
                : budget::Load::kDamaged;
    nvs_close(handle);
  } else if (opened != ESP_ERR_NVS_NOT_FOUND) {
    state = budget::Load::kDamaged;
  }
  discovery.load(state, record);
  discovery_progress = discovery.stored().progress;
}
bool storeDiscovery(const coverage::DiscoveryRecord& record) {
  std::array<uint8_t, coverage::record::kGuardBytes> bytes{};
  if (coverage::record::encodeGuard(record, bytes) != bytes.size())
    return false;
  nvs_handle_t handle;
  if (nvs_open(cfg::kBudgetNvsNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return false;
  auto err = nvs_set_blob(handle, cfg::kCoverageGuardNvsKey, bytes.data(),
                          bytes.size());
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err == ESP_OK;
}
bool reserveDiscovery(uint32_t utc_day) {
  if (discovery_retire) return false;
  using Admission = coverage::Discovery::Admission;
  coverage::DiscoveryRecord proposed;
  if (!discovery.stored().progress.active) {
    if (discovery.start(utc_day, proposed) != Admission::kStore) return false;
    proposed.binding = coverage_binding;
    if (!storeDiscovery(proposed)) return false;
    discovery.commitStart(proposed);
  }
  auto progress = discovery_progress;
  progress.active = true;
  progress.run_day = discovery.stored().progress.run_day;
  if (!discovery.stage(progress, proposed) || !storeDiscovery(proposed))
    return false;
  discovery.commitProgress(proposed);
  discovery_progress = progress;
  switch (discovery.request(proposed)) {
    case Admission::kAllowed: return true;
    case Admission::kStore:
      if (!storeDiscovery(proposed)) return false;
      discovery.commitRequest(proposed);
      return true;
    default: return false;
  }
}

bool saveDiscoveryProgress(const coverage::Progress& progress) {
  coverage::DiscoveryRecord proposed;
  if (!discovery.stage(progress, proposed) || !storeDiscovery(proposed))
    return false;
  discovery.commitProgress(proposed);
  discovery_progress = progress;
  return true;
}

bool storeCoverage(uint8_t pending_competition) {
  if (ota::trial() || files::state() != files::State::kMounted) return false;
  auto* bytes =
      new (std::nothrow) std::array<uint8_t, coverage::record::kRecordBytes>;
  if (bytes == nullptr) return false;
  lock();
  auto snapshot = discovery.stored();
  snapshot.binding = coverage_binding;
  const auto size = coverage::record::encode(coverage_evidence, snapshot,
                                             *bytes, pending_competition);
  const auto others = keptBytes(-1) - blocks(coverage_bytes.load());
  unlock();
  char path[files::kPathBufferBytes];
  footballPath(cfg::kCoverageFile, path, sizeof(path));
  const bool ok = size != 0 &&
                  others + blocks(size) <= cfg::kFootballFilesBytes &&
                  writeFile(path, size, [bytes, size](FILE* file) {
                    return fileWrite(bytes->data(), size, file) ? size : 0;
                  });
  delete bytes;
  if (ok) {
    coverage_bytes.store(size);
    if (pending_competition == 0) {
      lock();
      coverage_dirty = false;
      unlock();
    }
  }
  return ok;
}
bool fenceCoverage(uint8_t competition) {
  bool complete = false;
  lock();
  if (const auto* row = coverageOf(competition))
    for (const auto& span : row->seasons)
      complete |= span.completeness == coverage::Completeness::kComplete;
  unlock();
  if (complete && !storeCoverage(competition)) return false;
  lock();
  coverage_streaming = true;
  coverage_dirty |= complete;
  unlock();
  return true;
}

// Called with the heavy token during the existing kept-file load. Decoding
// directly into the sole owner is safe only while readers are excluded; any
// semantic/I/O failure clears that candidate before the lock is released.
void loadCoverage() {
  auto* bytes =
      new (std::nothrow) std::array<uint8_t, coverage::record::kRecordBytes>;
  if (bytes == nullptr) return;
  char path[files::kPathBufferBytes];
  footballPath(cfg::kCoverageFile, path, sizeof(path));
  struct stat info{};
  const auto actual_size = stat(path, &info) == 0 && info.st_size > 0
                               ? static_cast<uint32_t>(info.st_size)
                               : 0;
  FILE* file = fopen(path, "rb");
  bool ok = file != nullptr;
  if (file != nullptr) {
    ok = fread(bytes->data(), 1, bytes->size(), file) == bytes->size() &&
         fgetc(file) == EOF && !ferror(file);
    ok = fclose(file) == 0 && ok;
  }
  coverage::DiscoveryRecord snapshot;
  lock();
  ok = ok &&
       coverage::record::decode(*bytes, coverage_evidence, snapshot) ==
           coverage::record::DecodeResult::kOk &&
       snapshot.binding == coverage_binding;
  if (!ok) coverage_evidence = {};
  bindCoverage();
  unlock();
  // Even a corrupt file occupies quota until replacement/delete succeeds.
  coverage_bytes.store(actual_size);
  delete bytes;
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
  if (!sweep_pending || demo_mode || files::state() != files::State::kMounted)
    return;
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
      !writeFile(path, size,
                 [](FILE* file) { return names.encode(fileWrite, file); }))
    return false;
  names_bytes.store(size);
  return true;
}
// Worker, idle: writes at most one competition file with new data (every
// kCompetitionFileGapS at most, never during a live window) or the
// short-name file after a wipe. The worker alone writes the store, so it
// reads it without the mutex.
void writeKept(int64_t utc) {
  if (files::state() != files::State::kMounted || ota::trial() || demo_mode)
    return;
  const int64_t now = timekeeping::monotonicMs();
  if (coverage_dirty && !demo_mode &&
      (coverage_write_ms == 0 ||
       now - coverage_write_ms >= cfg::kStoreMaintenanceS * 1000LL)) {
    if (!network::tryHeavy()) return;
    const bool ok = storeCoverage();
    network::releaseHeavy();
    coverage_write_ms = ok ? 0 : now;
    if (ok) return;
    ESP_LOGW("provider", "schedule evidence not written");
    // Failure of optional coverage persistence must not starve the existing
    // short-name and competition-file owners of their idle write turns.
  }
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
  loadCoverage();
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
        slot.matches_loaded =
            saved.source
                .done[static_cast<std::size_t>(cfg::DataType::kFixtures)] > 0;
        slot.table_loaded =
            saved.source.done[static_cast<std::size_t>(cfg::DataType::kTable)] >
            0;
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
      slots[i].fetched_ms = slots[i].matches_ms =
          loadedMs(slots[i].restored_utc, utc, timekeeping::monotonicMs());
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
  coverage_bytes.store(0);
  coverage_dirty = true;
  unlock();
}

// Applies a new data generation: boot and a changed demo mode ('reset')
// drop every stored match and start every job afresh; else only
// competitions whose source changed, that were removed or paused,
// or that use a provider in 'providers' (its key or the ESPN opt-in
// changed) lose their data and jobs, the others keep theirs.
void applyRoutesLocked(const settings::Model& model, uint32_t generation,
                       bool reset, uint8_t providers = 0) {
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
  bindCoverage();
  for (auto& proof : coverage_evidence) {
    const auto* entry = routes.find(proof.id);
    if (entry != nullptr && usesAny(*entry, providers))
      coverage::invalidate(proof, coverage::Reason::kIdentity);
  }
  demo_mode = model.demo;
  if (!model.demo) demo_command.store(kNoDemoCommand);
  configured = model.demo;
  for (uint8_t i = 0; i < count; ++i)
    if (slots[i].active) {
      configured = true;
    }
  if (model.demo) {
    demo_clock = demo::apply(store, routes, demo_bindings, demo_scenario,
                             demo_step, nowClock().mono_s);
    const auto now = timekeeping::monotonicMs();
    for (auto& slot : slots) {
      slot.fetched_ms = slot.matches_ms = now;
      slot.matches_loaded = slot.table_loaded = true;
    }
  }
  store.collect();
  published_generation = generation;
  publish();
  demo_command.store(kNoDemoCommand);
}
void routesApplied() {
  // Files of removed, paused or changed competitions go (kept slots keep
  // 'on_file'; a reset clears it for every slot).
  sweep_pending = true;
  events::post(events::Event::kDataUpdated);
}
void applyRoutes(const settings::Model& model, uint32_t generation, bool reset,
                 uint8_t providers = 0) {
  lock();
  applyRoutesLocked(model, generation, reset, providers);
  unlock();
  routesApplied();
}
// Every kStoreMaintenanceS: estimated phases and minutes follow the clock,
// passed matchdays go (OQ-202), unused directory entries are freed.
void maintain(int64_t utc) {
  const auto dates = localDates(utc);
  lock();
  store.estimateAll(utc);
  for (uint8_t i = 0; i < route_count; ++i)
    if (slots[i].active)
      store.expire(slots[i].id, utc, dates.fallback_offset_s, &dates);
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
            uint32_t generation, uint32_t settings_generation, uint32_t epoch) {
  auto& entry = routes.entries[job.slot];
  lock();
  const bool has = planner.request(job, entry, store, nowClock(), request);
  unlock();
  if (!has) {
    scheduler.finished(
        job, schedule::Outcome::kNothing,
        static_cast<uint32_t>(timekeeping::monotonicMs() / 1000));
    return;
  }
  // Keep the existing token through complete(), which frees response event
  // staging. Policy HTTP must not overlap even this short completion window.
  network::HeavyGuard admission;
  busy.store(true);
  auto err =
      get(request.url, request.array, request.optional, job.provider, model,
          generation, settings_generation, epoch, consumeLocked,
          request.sink == plan::Sink::kMatches ? &entry : nullptr, nullptr,
          request.body, HttpPurpose::kAutomatic, nullptr, nullptr,
          request.sink == plan::Sink::kMatches ? entry.id : 0, &admission);
  busy.store(false);
  const auto clock = nowClock();
  const auto fetch = fetchOf(err);
  const auto dates = localDates(clock.utc);
  lock();
  const auto done = planner.complete(fetch, clock);
  if (done.invalid) {
    ESP_LOGW("provider", "no current season");
    err = stop(ESP_ERR_INVALID_RESPONSE, Cause::kNoSeason);
  }
  const auto outcome = done.outcome;
  const bool more = done.more, ok = outcome == schedule::Outcome::kOk;
  auto& slot = slots[job.slot];
  const bool matches =
      job.type == cfg::DataType::kFixtures || job.type == cfg::DataType::kLive;
  coverage_streaming = false;
  if (matches && sent && err != ESP_OK) {
    if (auto* proof = coverageOf(entry.id)) {
      coverage::invalidate(*proof, coverage::Reason::kPartialResponse);
      coverage_dirty = true;
    }
  }
  if (ok) {
    slot.fetched_ms = timekeeping::monotonicMs();
    if (matches) slot.matches_ms = slot.fetched_ms;
    slot.failed = false;
    slot.dirty = true;
    slot.restored_utc = 0;
    const auto type = static_cast<std::size_t>(job.type);
    if (!more && matches) slot.matches_loaded = true;
    if (!more && (job.type == cfg::DataType::kTable ||
                  job.type == cfg::DataType::kLiveTable))
      slot.table_loaded = true;
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
    if (matches && clock.utc > 0)
      store.expire(slot.id, clock.utc, dates.fallback_offset_s, &dates);
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
                   uint32_t settings_generation, int64_t utc,
                   int64_t& wake_ms) {
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
    err = get(url, key, false, request.provider, model, run.generation,
              settings_generation, run.epoch,
              request.provider == cfg::Provider::kOpenLigaDb ? seasonRow
                                                             : apiSeasonRow,
              &run, nullptr, cfg::kSelectionBodyBytes, HttpPurpose::kSetup);
    if (err == ESP_OK && run.season[0] == '\0') err = ESP_ERR_INVALID_RESPONSE;
  } else {
    SelectContext context{&request};
    err = get(url, key, false, request.provider, model, run.generation,
              settings_generation, run.epoch, selectionRow, &context,
              &context.enough, cfg::kSelectionBodyBytes, HttpPurpose::kSetup);
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

// Extra fixture discovery only gets the idle turn after all ordinary due/live
// jobs and explicit setup work. It uses the same parser, transport, limits and
// first-send admission; no separate request queue or provider resolver exists.
bool stepDiscovery(const settings::Model& model, uint32_t generation,
                   uint32_t settings_generation, uint32_t utc) {
  discovery_retire |= discovery.stored().progress.active &&
                      discovery.stored().binding != coverage_binding;
  if (discovery_retire) {
    if (discovery.stored().progress.active) {
      auto progress = discovery.stored().progress;
      progress.active = false;
      if (!saveDiscoveryProgress(progress)) return false;
    }
    discovery_retire = false;
    discovery_progress = {};
  }
  if (!eligibility.selection.automatic_break || eligibility.frame || utc == 0 ||
      routes.entries.empty())
    return false;
  const auto today = utc / 86400;
  const auto& stored = discovery.stored();
  if (!stored.progress.active && stored.progress.run_day != 0 &&
      today <= stored.progress.run_day)
    return false;
  if (!stored.progress.active) discovery_progress = {};
  if (stored.progress.active) discovery_progress = stored.progress;
  // Skip only already proven sources. A Partial/failed/stale source needs a
  // new complete response; old display rows or an eight-day list prove none.
  while (discovery_progress.route < route_count) {
    const auto route = discovery_progress.route;
    const auto& entry = routes.entries[route];
    if (!entry.active || entry.primary.competition.empty() ||
        !settings::providerUsable(entry.primary.provider, model)) {
      ++discovery_progress.route;
      discovery_progress.season = 0;
      continue;
    }
    const auto now_s = static_cast<uint32_t>(timekeeping::monotonicMs() / 1000);
    if (scheduler.admissibleAt(entry.primary.provider, now_s) > now_s)
      return false;
    // Reserve the same operation before allocating discovery state, and
    // retain it through proof persistence and release of that state.
    network::HeavyGuard admission;
    if (!admission) return false;
    auto* query = new (std::nothrow) coverage::Query;
    auto* response = new (std::nothrow) coverage::Response;
    if (query == nullptr || response == nullptr) {
      delete query;
      delete response;
      return false;
    }
    const bool prepared = coverage::discoveryQuery(
        entry, planner.state(route).season.view(), utc, eligibility.break_days,
        discovery_progress.season, *query);
    if (!prepared) {
      delete query;
      delete response;
      ++discovery_progress.route;
      discovery_progress.season = 0;
      continue;
    }
    response->begin(entry, *query, utc);
    busy.store(true);
    const auto err = get(
        query->http.url, query->http.array, query->http.optional,
        entry.primary.provider, model, generation, settings_generation,
        network::status().epoch,
        [](ArduinoJson::JsonVariantConst row, void* raw) {
          static_cast<coverage::Response*>(raw)->observe(row);
          return true;
        },
        response, nullptr, query->http.body, HttpPurpose::kDiscovery,
        response->observer(), &response->scan(), entry.id, &admission);
    busy.store(false);
    lock();
    coverage_streaming = false;
    unlock();
    if (!sent) {
      // DNS/TLS alone do not spend a discovery request. A reserved run that
      // reached its fixed cap closes instead of retrying indefinitely.
      delete response;
      delete query;
      if (failure_cause == Cause::kCounter &&
          discovery.stored().progress.active) {
        auto progress = discovery.stored().progress;
        progress.active = false;
        saveDiscoveryProgress(progress);
      }
      return false;
    }
    const auto clock = nowClock();
    const auto& span = response->finish(err == ESP_OK, clock.utc);
    lock();
    if (auto* proof = coverageOf(entry.id)) {
      coverage::accept(*proof, response->source(), span);
      coverage_dirty = true;
    }
    unlock();
    // Proof is unusable until its replacement has completed. Its first-send
    // NVS guard is already durable, independently of this file operation.
    storeCoverage();
    delete response;
    delete query;
    auto progress = discovery.stored().progress;
    if (++progress.season >= cfg::kCoverageMaxSeasons) {
      progress.season = 0;
      ++progress.route;
    }
    if (progress.route >= route_count) {
      progress.route = 0;
      progress.active = false;
    }
    saveDiscoveryProgress(progress);
    lock();
    publish();
    unlock();
    events::post(events::Event::kDataUpdated);
    return true;
  }
  if (discovery.stored().progress.active) {
    auto progress = discovery.stored().progress;
    progress.active = false;
    saveDiscoveryProgress(progress);
  }
  return false;
}
// Fill the worker's existing storage from one locked generation. Separately
// reading model/routes/counters could mix a concurrent competition reorder
// with another snapshot's positions and identities.
void reloadLocked(settings::Model& model, uint32_t& settings_generation,
                  uint32_t& data_generation) {
  settings::footballSettings(
      model, routes, settings_generation, data_generation, &eligibility,
      [](const settings::Model& snapshot, settings::Competitions& effective,
         bool saved_competitions) {
        if (snapshot.demo)
          demo_bindings =
              demo::configure(effective, demo_scenario, saved_competitions);
        else
          demo_bindings = {};
      });
  coverage_binding = coverageBinding(model);
}
void reload(settings::Model& model, uint32_t& settings_generation,
            uint32_t& data_generation) {
  // Readers borrow routes. Keep its replacement under the same repository
  // mutex; their generation check rejects a not-yet-applied data generation.
  lock();
  reloadLocked(model, settings_generation, data_generation);
  unlock();
}
void setCadence(Cadence cadence, int64_t due) {
  lock();
  cadence_now = cadence;
  due_ms = due;
  unlock();
}
void worker(void*) {
  uint32_t settings_seen = 0, applied = 0;
  settings::Model model;
  loadDiscovery();
  reload(model, settings_seen, applied);
  Access access = accessOf(model);
  applyRoutes(model, applied, true);
  if (!model.demo) loadKept();
  int64_t maintained_ms = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
    relost();
    // Read before the copy: a save in between then interrupts the request.
    auto generation = settings::dataGeneration();
    if (const auto seen = settings::generation();
        seen != settings_seen || generation != applied) {
      const bool demo = model.demo;
      const Access prior_access = access;
      lock();
      reloadLocked(model, settings_seen, generation);
      access = accessOf(model);
      const bool data_changed = generation != applied;
      if (data_changed) {
        discovery_retire = true;
        applyRoutesLocked(model, generation, model.demo != demo,
                          changedProviders(prior_access, access));
        applied = generation;
      }
      unlock();
      if (data_changed) {
        routesApplied();
        if (demo && !model.demo) loadKept();
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
    if (!model.demo &&
        (maintained_ms == 0 ||
         now - maintained_ms >= cfg::kStoreMaintenanceS * 1000LL)) {
      maintained_ms = now;
      maintain(utc);
    }
    // The owner's selection goes first; while it waits for its provider,
    // the jobs run. Configure limits independently of automatic next(): in
    // frame mode that path stays paused, but wizard limits must still apply.
    scheduler.configure(model.providers, model.global_limit);
    int64_t selection_wake = INT64_MAX;
    if (selecting.load() &&
        stepSelection(model, generation, settings_seen, utc, selection_wake))
      continue;
    if (model.demo) {
      const auto command = demo_command.load();
      if (command != kNoDemoCommand) {
        lock();
        const bool changed =
            demo_scenario != static_cast<demo::Scenario>(command >> 8);
        demo_scenario = static_cast<demo::Scenario>(command >> 8);
        demo_step = static_cast<uint8_t>(command);
        ESP_LOGI(kTag, "demo scenario %u step %u",
                 static_cast<unsigned>(demo_scenario), demo_step);
        if (changed) {
          // Publish identity, policy association, fixture data and test clock
          // together. Readers cannot combine the new scenario's proof with
          // the previous scenario's store while projection is in progress.
          reloadLocked(model, settings_seen, generation);
          applyRoutesLocked(model, generation, true);
          unlock();
          access = accessOf(model);
          applied = generation;
          routesApplied();
          if (!model.demo) loadKept();
          continue;
        }
        demo_clock = demo::apply(store, routes, demo_bindings, demo_scenario,
                                 demo_step, nowClock().mono_s);
        publish();
        demo_command.store(kNoDemoCommand);
        unlock();
        events::post(events::Event::kDataUpdated);
      }
      setCadence(Cadence::kIdle, 0);
      continue;
    }
    if (settings::footballPaused()) {
      // The explicit setup flow above retains all of its normal limits and
      // retries. Automatic jobs, including pending Planner continuations,
      // stay untouched until frame ends; no forced refresh is queued for it.
      requested.store(false);
      sweep();
      writeKept(utc);
      lock();
      cadence_now = Cadence::kNone;
      due_ms = retry_ms = 0;
      unlock();
      continue;
    }
    if (seed_pending) {
      seed_pending = false;
      seedKept(utc);
    }
    if (requested.exchange(false)) scheduler.refresh();
    const auto decision_clock = nowClock();
    const auto decision_dates = localDates(utc);
    const bool break_proven =
        !coverage_streaming && !coverage_dirty &&
        coverage::assess(coverage_evidence, routes, utc, eligibility.break_days)
                .verdict == coverage::Verdict::kBreak;
    const selection::Input selection_input{
        eligibility.selection, routes,         store,
        decision_clock,        decision_dates, break_proven};
    const auto selected = selection::select(selection_input);
    planner.context(routes, eligibility, decision_dates, break_proven);
    for (uint8_t i = 0; i < cfg::kCompetitionMax; ++i)
      planner.wanted(i,
                     i < routes.entries.size() ? &routes.entries[i] : nullptr,
                     store, decision_clock, inputs.competitions[i], &selected);
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
      runJob(decision.job, model, generation, settings_seen,
             network::status().epoch);
      continue;
    }
    if (stepDiscovery(model, generation, settings_seen, utc)) continue;
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
      [](events::Event, events::Payload, void*) { time_changed.store(true); },
      nullptr);
  if (subscription != ESP_OK) return subscription;
  return xTaskCreateStatic(worker, "provider", cfg::kProviderStackBytes,
                           nullptr, 3, stack, &task_buffer)
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}
void refresh() {
  // An explicit setup list is the frame exception; a refresh must not turn
  // every job into overdue work when normal football operation resumes.
  if (!settings::footballPaused()) requested.store(true);
}
namespace {
void statusLocked(Status& s, int64_t utc, int64_t now) {
  s.started = true;
  s.revision = revision;
  s.generation = published_generation;
  memcpy(s.requests, requests, sizeof(requests));
  s.fetched_ms = oldestFetch();
  s.matches = static_cast<uint8_t>(
      std::min<std::size_t>(UINT8_MAX, store.matchCount()));
  s.competitions = states;
  s.stale = markStale(s.competitions, utc, now);
  s.cadence = cadence_now;
  const bool scheduled = cadence_now != Cadence::kNone &&
                         cadence_now != Cadence::kWaiting &&
                         cadence_now != Cadence::kUpdateTest;
  s.next_ms = scheduled ? due_ms : 0;
  s.busy = busy.load();
}
}  // namespace
Status status() {
  Status s{};
  if (mutex == nullptr) return s;
  time_t utc = 0;
  const bool valid = timekeeping::utcNow(utc);
  const int64_t now = timekeeping::monotonicMs();
  lock();
  statusLocked(s, valid ? utc : 0, now);
  unlock();
  return s;
}
DemoStatus demoStatus() {
  DemoStatus status;
  if (mutex == nullptr) return status;
  lock();
  status.enabled = demo_mode;
  status.pending = demo_command.load() != kNoDemoCommand;
  status.scenario = demo_scenario;
  status.step = demo_step;
  status.revision = revision;
  status.clock = demo_clock;
  if (demo_mode) status.bindings = demo_bindings;
  unlock();
  return status;
}
esp_err_t demoScenario(demo::Scenario scenario, uint8_t step) {
  if (!demo::valid(scenario) || step >= demo::steps(scenario))
    return ESP_ERR_INVALID_ARG;
  if (mutex == nullptr) return ESP_ERR_INVALID_STATE;
  lock();
  const bool enabled =
      demo_mode && published_generation == settings::dataGeneration();
  uint16_t expected = kNoDemoCommand;
  const bool queued =
      enabled &&
      demo_command.compare_exchange_strong(
          expected,
          static_cast<uint16_t>(static_cast<unsigned>(scenario) << 8 | step));
  unlock();
  return queued ? ESP_OK : enabled ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
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
  uint8_t loaded_matches = 0, loaded_tables = 0;
  for (uint8_t i = 0; i < route_count; ++i) {
    if (slots[i].matches_loaded || demo_mode) loaded_matches |= 1u << i;
    if (slots[i].table_loaded || demo_mode) loaded_tables |= 1u << i;
  }
  const Published data{
      store,
      shown,
      names,
      revision,
      published_generation,
      configured,
      stale,
      oldestFetch(),
      retry_ms,
      states,
      routes,
      demo_mode ? demo::breakProven(demo_scenario, eligibility.break_days)
                : !coverage_streaming && !coverage_dirty && valid &&
                      coverage::assess(coverage_evidence, routes, utc,
                                       eligibility.break_days)
                              .verdict == coverage::Verdict::kBreak,
      loaded_matches,
      loaded_tables,
      demo_mode ? demo_clock : nowClock()};
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
Overview overview(int64_t utc, Status* status_out) {
  Overview result{};
  if (status_out != nullptr) status_out->started = false;
  if (mutex == nullptr) return result;
  const int64_t now = status_out != nullptr ? timekeeping::monotonicMs() : 0;
  lock();
  if (status_out != nullptr) statusLocked(*status_out, utc, now);
  result.demo = demo_mode;
  if (demo_mode) {
    utc = demo_clock.utc;
    result.demo_bindings = demo_bindings;
  }
  for (uint8_t i = 0; i < route_count; ++i) {
    const auto& slot = slots[i];
    auto& info = result.routes[i];
    info.id = slot.id;
    if (!slot.active) continue;
    info.matches = static_cast<uint8_t>(
        std::min<uint16_t>(UINT8_MAX, view::matchCount(store, shown, slot.id)));
    if (const auto* source = shown.source(slot.id))
      info.provider = source->provider;
  }
  const int best = view::ownForOverview(store, shown, utc);
  if (best >= 0) {
    const auto& m = store.match(best);
    auto& own = result.own;
    own.present = true;
    own.competition = m.competition;
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
  footballPath(cfg::kCoverageFile, path, sizeof(path));
  unlink(path);
}
}  // namespace football
