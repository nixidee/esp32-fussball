#include "football_service.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <type_traits>

#include "binary_codec.h"
#include "civil_time.h"
#include "esp_log.h"
#include "esp_random.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_service.h"
#include "nvs.h"
#include "original_names_channel.h"
#include "ota_service.h"
#include "provider_client.h"
#include "provider_coverage.h"
#include "provider_plan.h"
#include "provider_settings.h"
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
std::atomic<uint32_t> original_query{0};
uint32_t original_generation = 0, original_settings_generation = 0,
         original_epoch = 0;
// Query owns the active type; rows are constructed only when admitted.
struct SelectionRows {
  union Row {
    CompetitionSelection competition;
    TeamSelection team;
    Row() noexcept {}
  } rows[cfg::kSelectionEntries];
  void set(std::size_t at, const CompetitionSelection& value) {
    new (&rows[at].competition) CompetitionSelection(value);
  }
  void set(std::size_t at, const TeamSelection& value) {
    new (&rows[at].team) TeamSelection(value);
  }
  void get(std::size_t at, CompetitionSelection& value) const {
    value = rows[at].competition;
  }
  void get(std::size_t at, TeamSelection& value) const {
    value = rows[at].team;
  }
} selections;
SelectionStatus selection_state{};
uint32_t selection_boot_nonce = 0, selection_counter = 0,
         selection_settings_generation = 0;
int64_t selection_deadline_ms = 0;
bool selection_cancelled = false;

MatchStore store;
view::Shown shown;
// Worker only.
plan::Planner planner;
schedule::Scheduler scheduler;
// Run member defaults during startup instead of retaining whole-object flash
// images. These worker-owned types add no fields or storage and keep the
// public projection types aggregate-initialisable for their other callers.
struct WorkerInputs : schedule::Inputs {
  WorkerInputs() noexcept {}
};
WorkerInputs inputs;
plan::Request request;
// The worker's copy of the competition store (index = route = scheduler
// slot), refreshed with its narrow settings snapshot.
struct WorkerRoutes : settings::CompetitionRoutes {
  WorkerRoutes() noexcept {}
};
WorkerRoutes routes;
settings::screen_policy::Eligibility eligibility;
struct WorkerCoverage : coverage::EvidenceArray {
  WorkerCoverage() noexcept {}
};
WorkerCoverage coverage_evidence;
coverage::Discovery discovery;
bool coverage_streaming = false;
bool discovery_retire = false;
struct DiscoveryCursor {
  uint8_t route = 0, season = 0;
};
DiscoveryCursor discovery_progress;
uint64_t discovery_binding = 0;
uint64_t coverage_binding = 0;

bool reserveDiscovery(uint32_t utc_day);
coverage::Evidence* coverageOf(uint8_t id);

// Per route (mutex-protected; written by the worker). 'key' hashes the
// competition's source: a changed key drops its stored data.
struct Slot {
  uint8_t id = 0;
  bool active = false;
  uint32_t key = 0;
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
// Reset coordination is independent of names. Provider mapping always keeps
// its compact original fallback and never opens a names file.
bool reset_fence = false, worker_active = false;

void lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
void unlock() { xSemaphoreGive(mutex); }
class WorkerActivity {
 public:
  WorkerActivity() {
    lock();
    held_ = !reset_fence && !worker_active;
    if (held_) worker_active = true;
    unlock();
  }
  ~WorkerActivity() { release(); }
  explicit operator bool() const { return held_; }
  void release() {
    if (!held_) return;
    lock();
    worker_active = false;
    unlock();
    held_ = false;
  }

 private:
  bool held_ = false;
};
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
  const char separator = static_cast<char>(0xff);
  return values::fnv1a32({&separator, 1}, values::fnv1a32(bytes, hash));
}
uint32_t fnv(uint32_t hash, uint32_t value) {
  uint8_t bytes[4];
  binary::little(bytes, value);
  return fnv(hash, {reinterpret_cast<const char*>(bytes), sizeof(bytes)});
}
uint32_t sourceHash(uint32_t hash, const settings::Source& s) {
  hash = fnv(hash, static_cast<uint32_t>(s.provider));
  hash = fnv(hash, s.competition.view());
  return fnv(hash, s.season.view());
}
// What the stored data of a competition depends on: its service-owned
// identity and source. A reused small id must not retain an old slot.
uint32_t routeKey(const settings::CompetitionRoute& e) {
  return sourceHash(fnv(fnv(values::kFnv1a32Basis, e.id), e.incarnation),
                    e.primary);
}
// Per provider, what its access depends on: its key (API-Football,
// football-data.org) or the ESPN opt-in. A change drops the data of the
// competitions that use that provider.
using Access = std::array<uint32_t, cfg::kProviderCount>;
Access accessOf(const settings::ProviderSettings& m) {
  Access out{};
  out[static_cast<std::size_t>(cfg::Provider::kApiFootball)] =
      fnv(values::kFnv1a32Basis, m.api_football_key.view());
  out[static_cast<std::size_t>(cfg::Provider::kFootballData)] =
      fnv(values::kFnv1a32Basis, m.football_data_key.view());
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
uint64_t coverageBinding(const settings::ProviderSettings& model) {
  uint32_t low = values::kFnv1a32Basis, high = 0x9e3779b9u;
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
bool usesAny(const settings::CompetitionRoute& e, uint8_t providers) {
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
int64_t sent_ms = 0;
struct SendContext {
  cfg::Provider provider;
  const settings::ProviderSettings& model;
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
  const auto send_day =
      static_cast<uint32_t>(utc / timekeeping::kSecondsPerDay);
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
  auto err = budget::admit(send.provider, day,
                           send.model.providers[index].limit.daily);
  if (err == ESP_OK) err = budget::admitGlobal(day, global.daily);
  if (err != ESP_OK)
    return refuse(
        err, err == ESP_ERR_NOT_ALLOWED ? Cause::kBudget : Cause::kCounter);
  sent_ms = timekeeping::monotonicMs();
  scheduler.sent(send.provider, static_cast<uint32_t>(sent_ms / 1000));
  lock();
  if (send.competition != 0) coverage_streaming = true;
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
esp_err_t get(const provider::RequestSpec& spec,
              const settings::ProviderSettings& m, uint32_t generation,
              uint32_t settings_generation, uint32_t epoch,
              const provider::Sink& sink, provider::ParseResult& parsed,
              HttpPurpose purpose = HttpPurpose::kAutomatic,
              uint8_t pending_competition = 0,
              const network::HeavyGuard* admission = nullptr) {
  const auto p = spec.source.provider;
  char label[80];
  snprintf(label, sizeof(label), "%u/%.*s operation %u", unsigned(p),
           int(spec.source.competition.size()), spec.source.competition.data(),
           unsigned(spec.operation));
  const char* url = label;
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
  const auto query = purpose == HttpPurpose::kSetup ? original_query.load() : 0;
  const SendHooks send_hooks{
      beginSend, endSend, &send_context, purpose != HttpPurpose::kSetup,
      query ? static_cast<int64_t>(originals::deadlineMs(query)) : 0};
  FetchResult result;
  const int64_t started = timekeeping::monotonicMs();
  const auto err =
      fetchProvider(spec, sink, m, generation, parsed, &result, &send_hooks);
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
// Canonical mapping is pure under the repository mutex. Adopted labels are
// resolved only for visible output after releasing this lock.
bool consumeLocked(const provider::Fixture& fixture,
                   std::span<const provider::Event> events, void* raw) {
  lock();
  if (raw && planner.acceptsMatch(fixture)) {
    const auto& entry = *static_cast<const settings::CompetitionRoute*>(raw);
    if (auto* evidence = coverageOf(entry.id))
      coverage::observeNormalRow(*evidence, entry, fixture);
  }
  const bool ok = planner.consume(fixture, events);
  unlock();
  return ok;
}
provider::Sink plannerSink(const settings::CompetitionRoute& entry) {
  provider::Sink sink;
  sink.context = const_cast<settings::CompetitionRoute*>(&entry);
  sink.fixture = consumeLocked;
  sink.row = [](const provider::TableRow& row, void*) {
    lock();
    const bool ok = planner.consume(row);
    unlock();
    return ok;
  };
  sink.group = [](const provider::TableGroup& group, void*) {
    lock();
    const bool ok = planner.consume(group);
    unlock();
    return ok;
  };
  sink.event = [](const provider::Event& event, void*) {
    lock();
    const bool ok = planner.consume(event);
    unlock();
    return ok;
  };
  sink.day = [](uint32_t day, void*) { return planner.consumeDay(day); };
  sink.text = [](std::string_view value, void*) {
    return planner.consumeText(value);
  };
  sink.calendar_round = [](const provider::CalendarRound& round, void*) {
    return planner.consumeCalendarRound(round);
  };
  return sink;
}

// Under the mutex: the shown sources and the competition states after the
// store or the slots changed; a new revision.
void publish() {
  shown = {};
  for (const auto& entry : routes.entries) shown.add(entry);
  error_routes = 0;
  states = {};
  const auto clock = demo_mode ? demo_clock : nowClock();
  for (uint8_t i = 0; i < route_count; ++i) {
    const auto& slot = slots[i];
    auto& state = states[i];
    state.id = slot.id;
    state.active = slot.active;
    const auto summary = store.datasetSummary(slot.id);
    state.fetched_ms = summary.last_success_ms;
    if (!slot.active) continue;
    if (summary.cause != Cause::kNone) error_routes |= 1u << i;
    state.notices = store.notices(slot.id);
    state.cause = summary.cause != Cause::kNone ? summary.cause
                  : store.unmaintained(slot.id, clock.utc)
                      ? Cause::kUnmaintained
                  : state.notices ? Cause::kStoreFull
                                  : Cause::kNone;
    state.provider = routes.entries[i].primary.provider;
    state.http = summary.http;
  }
  ++revision;
}
// Under the mutex: oldest last success over the active competitions
// (0 = none yet).
int64_t oldestFetch() {
  int64_t oldest = 0;
  for (uint8_t i = 0; i < route_count; ++i)
    if (slots[i].active) {
      const auto last = store.datasetSummary(slots[i].id).last_success_ms;
      if (last && (!oldest || last < oldest)) oldest = last;
    }
  return oldest;
}
// Under the mutex: marks the competitions of 'out' whose last request
// failed, or with a shown match in its live window while the competition's
// last successful match request is older than kDataStaleS; true if any.
// 'utc' 0 = no clock.
bool markStale(CompetitionStates& out, int64_t utc, int64_t now_ms) {
  bool any = false;
  for (uint8_t i = 0; i < route_count; ++i) {
    out[i].stale = false;
    if (!demo_mode && slots[i].active)
      for (uint8_t dataset = 0; dataset < 3; ++dataset)
        out[i].stale |= schedule::datasetStale(
            store, slots[i].id, static_cast<MatchStore::Dataset>(dataset), utc,
            now_ms, routes.entries[i].primary.provider);
    any |= out[i].stale;
  }
  return any;
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
}

// Only discovery run/request credits persist in NVS. Schedule evidence is
// volatile and must be reacquired after a restart.
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
  discovery_progress = {};
  discovery_binding = 0;
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
  if (!discovery.stored().active) {
    if (discovery.start(utc_day, proposed) != Admission::kStore) return false;
    if (!storeDiscovery(proposed)) return false;
    discovery.commitStart(proposed);
    discovery_binding = coverage_binding;
  }
  switch (discovery.request(proposed)) {
    case Admission::kAllowed: return true;
    case Admission::kStore:
      if (!storeDiscovery(proposed)) return false;
      discovery.commitRequest(proposed);
      return true;
    default: return false;
  }
}

bool finishDiscovery() {
  coverage::DiscoveryRecord proposed;
  if (discovery.stored().active) {
    if (!discovery.finish(proposed) || !storeDiscovery(proposed)) return false;
    discovery.commitFinish(proposed);
  }
  discovery_progress = {};
  discovery_binding = coverage_binding;
  return true;
}

// Applies a new data generation: boot and a changed demo mode ('reset')
// drop every stored match and start every job afresh; else only
// competitions whose source changed, that were removed or paused,
// or that use a provider in 'providers' (its key or the ESPN opt-in
// changed) lose their data and jobs, the others keep theirs.
void applyRoutesLocked(const settings::ProviderSettings& model,
                       uint32_t generation, bool reset, uint8_t providers = 0) {
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
    if (!kept && entry.id != 0)
      store.removeCompetition(entry.id);
    else if (kept)
      store.selectClub(entry.id, entry.primary.provider, entry.club.team,
                       nowClock());
    if (next[i].active && !model.demo) {
      const auto supported = provider::capabilities(entry.primary.provider);
      store.datasetAvailable(entry.id, MatchStore::Dataset::kFixtures,
                             supported.fixtures);
      store.datasetAvailable(entry.id, MatchStore::Dataset::kTable,
                             supported.table);
      store.datasetAvailable(entry.id, MatchStore::Dataset::kEvents,
                             supported.events);
    }
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
      for (const auto dataset :
           {MatchStore::Dataset::kFixtures, MatchStore::Dataset::kTable,
            MatchStore::Dataset::kEvents})
        store.recordDataset(slot.id, dataset, 0, now, true, Cause::kNone);
    }
  }
  store.collect();
  published_generation = generation;
  publish();
  demo_command.store(kNoDemoCommand);
}
void routesApplied() { events::post(events::Event::kDataUpdated); }
void applyRoutes(const settings::ProviderSettings& model, uint32_t generation,
                 bool reset, uint8_t providers = 0) {
  lock();
  applyRoutesLocked(model, generation, reset, providers);
  unlock();
  routesApplied();
}
// Every kStoreMaintenanceS: estimated phases/minutes follow the clock and
// unused references are freed. Authoritative fixture runs alone prune scope.
void maintain(int64_t utc) {
  lock();
  store.estimateAll(utc);
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
void runJob(const schedule::Job& job, const settings::ProviderSettings& model,
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
  provider::ParseResult parsed;
  const auto sink = plannerSink(entry);
  auto err = get(request.spec, model, generation, settings_generation, epoch,
                 sink, parsed, HttpPurpose::kAutomatic,
                 (request.sink == plan::Sink::kMatches ||
                  request.sink == plan::Sink::kIndex ||
                  request.sink == plan::Sink::kOwnIndex ||
                  request.sink == plan::Sink::kOwnMatches)
                     ? entry.id
                     : 0,
                 &admission);
  lock();
  const bool response_valid = planner.response(parsed);
  unlock();
  if (!response_valid) {
    const auto failure = planner.responseFailure();
    if (failure == provider::Status::kNoMemory)
      err = stop(ESP_ERR_NO_MEM, Cause::kMemory);
    else if (failure == provider::Status::kTooLarge)
      err =
          stop(ESP_ERR_INVALID_SIZE,
               store.notices(entry.id) ? Cause::kStoreFull : Cause::kTooLarge);
    else if (err == ESP_OK)
      err = failure == provider::Status::kRejected
                ? stop(ESP_ERR_INVALID_RESPONSE, Cause::kBadResponse)
                : stop(ESP_ERR_INVALID_SIZE, Cause::kTooLarge);
  }
  busy.store(false);
  const auto clock = nowClock();
  const auto fetch = fetchOf(err);
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
    }
  }
  const auto dataset = matches ? MatchStore::Dataset::kFixtures
                       : job.type == cfg::DataType::kEvents
                           ? MatchStore::Dataset::kEvents
                           : MatchStore::Dataset::kTable;
  if (ok) {
    store.recordDataset(slot.id, dataset, sent ? sent_ms : 0,
                        timekeeping::monotonicMs(), !more, Cause::kNone);
  } else if (outcome != schedule::Outcome::kDeferred ||
             fetch == plan::Fetch::kCapped) {
    // Local busy/settings/window refusals do not become provider attempts.
    // A budget refusal stays visible but keeps the actual send timestamp.
    store.recordDataset(slot.id, dataset, sent ? sent_ms : 0, 0, false,
                        causeOf(err), httpOf(err));
  }
  publish();
  unlock();
  // A sent request counts from its start, a refusal from now.
  scheduler.finished(
      job, outcome,
      sent ? static_cast<uint32_t>(sent_ms / 1000)
           : static_cast<uint32_t>(timekeeping::monotonicMs() / 1000),
      more);
  events::post(events::Event::kDataUpdated);
}

bool selectionCurrentLocked(SelectionQuery query, int64_t now_ms) {
  return query != 0 && query == selection_state.query && !selection_cancelled &&
         now_ms < selection_deadline_ms &&
         settings::generation() == selection_settings_generation;
}
bool selectionAlive(SelectionQuery query) {
  if (!mutex) return false;
  lock();
  const bool alive = selectionCurrentLocked(query, timekeeping::monotonicMs());
  unlock();
  return alive;
}
int64_t selectionDeadline() {
  lock();
  const auto deadline = selection_state.busy && !original_query.load()
                            ? selection_deadline_ms
                            : INT64_MAX;
  unlock();
  return deadline;
}
struct SelectContext {
  const SelectionRequest* request;
  SelectionQuery query;
  unsigned seen = 0;
  bool enough = false;
};
// Case folding needed by the provider selector: ASCII and Latin-1 capitals.
// It changes only the borrowed search copy, never canonical provider names.
template <std::size_t N>
void fold(char (&text)[N]) {
  for (std::size_t i = 0; i < N && text[i]; ++i) {
    auto& ch = reinterpret_cast<unsigned char&>(text[i]);
    if (ch >= 'A' && ch <= 'Z')
      ch += 32;
    else if (ch == 0xc3 && i + 1 < N) {
      auto& next = reinterpret_cast<unsigned char&>(text[i + 1]);
      if (next >= 0x80 && next <= 0x9e && next != 0x97) next += 0x20;
      if (next) ++i;
    }
  }
}
template <class Row>
bool selectionRow(const Row& selection, void* context) {
  auto& page = *static_cast<SelectContext*>(context);
  if (!selectionAlive(page.query)) return false;
  const auto& query = *page.request;
  if (query.teams != std::is_same_v<Row, TeamSelection>) return false;
  if (!query.search.empty()) {
    char hay[cfg::kNameBytes + 1]{}, needle[cfg::kSelectionTextMaxBytes + 1]{};
    values::copyPadded(hay, sizeof(hay), selection.name.data);
    values::copyPadded(needle, sizeof(needle), query.search.data);
    fold(hay);
    fold(needle);
    if (strstr(hay, needle) == nullptr) return true;
  }
  if (page.seen++ < query.offset) return true;
  lock();
  if (!selectionCurrentLocked(page.query, timekeeping::monotonicMs())) {
    unlock();
    return false;
  }
  if (selection_state.count >= cfg::kSelectionEntries)
    selection_state.more = page.enough = true;
  else
    selections.set(selection_state.count++, selection);
  unlock();
  return true;
}
void finishSelection(SelectionQuery query, esp_err_t err) {
  lock();
  if (query == 0 || query != selection_state.query || original_query.load()) {
    unlock();
    return;
  }
  if (err != ESP_OK ||
      !selectionCurrentLocked(query, timekeeping::monotonicMs())) {
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
  selecting.store(false);
  unlock();
}
// One interactive operation on the existing worker and admission path. A
// missing source season is discovered first; no URL or provider JSON knowledge
// belongs in this owner. A brief failed list request gets one admitted retry.
struct SelectionRun {
  enum Stage : uint8_t { kSeason, kList, kRetry };
  bool active = false;
  SelectionQuery query = 0;
  Stage stage = kList;
  uint32_t generation = 0, epoch = 0;
  int64_t busy_since_ms = 0;
  Id season;
};
SelectionRun run;
void endSelection(SelectionQuery query, esp_err_t err) {
  if (run.query == query) run = {};
  finishSelection(query, err);
}
void setSelectionWait(SelectionQuery query, Cadence wait, int64_t wait_ms) {
  lock();
  if (selectionCurrentLocked(query, timekeeping::monotonicMs())) {
    selection_state.wait = wait;
    selection_state.wait_ms = wait_ms;
  }
  unlock();
}
bool stepSelection(const settings::ProviderSettings& model, uint32_t generation,
                   uint32_t settings_generation, int64_t utc,
                   int64_t& wake_ms) {
  SelectionRequest query;
  SelectionQuery owner;
  lock();
  query = selection_request;
  owner = selection_state.query;
  const bool alive = selectionCurrentLocked(owner, timekeeping::monotonicMs());
  unlock();
  if (!alive) {
    endSelection(owner, ESP_ERR_INVALID_STATE);
    return true;
  }
  if (run.active && run.query != owner) {
    endSelection(run.query, ESP_ERR_INVALID_STATE);
    return true;
  }
  if (!run.active) {
    run = {};
    run.active = true;
    run.query = owner;
    run.generation = generation;
    run.epoch = network::status().epoch;
    provider::SelectionPlan preparation;
    if (!provider::prepareSelection(
            {query.provider, query.competition.data, query.season.data},
            query.teams ? provider::Operation::kTeams
                        : provider::Operation::kCompetitions,
            utc, preparation)) {
      endSelection(owner, ESP_ERR_INVALID_ARG);
      return true;
    }
    run.season = preparation.season;
    run.stage =
        preparation.discover ? SelectionRun::kSeason : SelectionRun::kList;
  }
  const int64_t now_ms = timekeeping::monotonicMs();
  const auto now_s = static_cast<uint32_t>(now_ms / 1000);
  schedule::Wait why = schedule::Wait::kIdle;
  const auto at = scheduler.admissibleAt(query.provider, now_s, &why);
  if (at > now_s) {
    const auto shown = why == schedule::Wait::kWindow   ? Cadence::kLimit
                       : why == schedule::Wait::kPaused ? Cadence::kRetry
                                                        : Cadence::kNone;
    wake_ms = at * 1000LL;
    setSelectionWait(owner, shown, shown == Cadence::kNone ? 0 : wake_ms);
    return false;
  }
  setSelectionWait(owner, Cadence::kNone, 0);
  const provider::RequestSpec request{
      {query.provider, query.competition.data, run.season.data},
      run.stage == SelectionRun::kSeason ? provider::Operation::kCurrentSeason
      : query.teams                      ? provider::Operation::kTeams
                                         : provider::Operation::kCompetitions,
      0,
      0,
      0,
      0,
      0,
      query.country.data};
  provider::ParseResult parsed;
  SelectContext context{&query, owner};
  provider::Sink sink{&context};
  sink.competition_selection = selectionRow<CompetitionSelection>;
  sink.team_selection = selectionRow<TeamSelection>;
  sink.cancelled = [](void* value) {
    return !selectionAlive(static_cast<SelectContext*>(value)->query);
  };
  sink.enough = [](void* value) {
    return static_cast<SelectContext*>(value)->enough;
  };
  busy.store(true);
  const auto err = get(request, model, run.generation, settings_generation,
                       run.epoch, sink, parsed, HttpPurpose::kSetup);
  busy.store(false);
  if (!selectionAlive(owner)) {
    endSelection(owner, ESP_ERR_INVALID_STATE);
    return true;
  }
  if (fetchOf(err) == plan::Fetch::kBusy) {
    if (run.busy_since_ms == 0) run.busy_since_ms = now_ms;
    if (now_ms - run.busy_since_ms < cfg::kOperationDeadlineMs) {
      wake_ms = now_ms + cfg::kRequestGapMs;
      return false;
    }
    endSelection(owner, err);
    return true;
  }
  run.busy_since_ms = 0;
  if (err == ESP_OK && run.stage == SelectionRun::kSeason) {
    if (parsed.metadata.season.empty()) {
      endSelection(owner, ESP_ERR_INVALID_RESPONSE);
      return true;
    }
    run.season = parsed.metadata.season;
    run.stage = SelectionRun::kList;
    return true;
  }
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
  endSelection(owner, err);
  return true;
}

struct OriginalsRun {
  settings::Source source;
  bool claimed = false, season_ready = false;
};
OriginalsRun original_run;
void endOriginalNames(uint32_t query, esp_err_t err) {
  originals::complete(query, err);
  lock();
  if (original_query.load() != query) {
    unlock();
    return;
  }
  original_run = {};
  run = {};
  original_query.store(0);
  selecting.store(false);
  unlock();
}
struct OriginalEncoder {
  uint32_t query;
  const settings::Source& source;
  json::Writer writer;
  bool begun = false;
  char buffer[128]{};
  std::size_t used = 0;
  bool flush() {
    if (!used) return true;
    const bool ok = originals::write(query, {buffer, used});
    used = 0;
    return ok;
  }
  bool push(std::string_view bytes) {
    for (char byte : bytes) {
      buffer[used++] = byte;
      if (used == sizeof(buffer) && !flush()) return false;
    }
    return true;
  }
  OriginalEncoder(uint32_t id, const settings::Source& value)
      : query(id),
        source(value),
        writer({this, [](std::string_view bytes, void* p) {
                  auto& self = *static_cast<OriginalEncoder*>(p);
                  return self.push(bytes);
                }}) {}
  bool begin() {
    if (begun) return true;
    begun = true;
    return writer.object() && writer.field("query", query) &&
           writer.key("source") && writer.object() &&
           writer.field("provider", static_cast<unsigned>(source.provider)) &&
           writer.field("competition", source.competition.view()) &&
           writer.field("season", source.season.view()) && writer.end() &&
           writer.key("items") && writer.array();
  }
  provider::OriginalsSink sink() {
    return {
        this,
        [](void* p) {
          auto& self = *static_cast<OriginalEncoder*>(p);
          return self.begin() && self.writer.object();
        },
        [](uint32_t id, void* p) {
          return static_cast<OriginalEncoder*>(p)->writer.field("id", id);
        },
        [](provider::OriginalField field, void* p) {
          auto& writer = static_cast<OriginalEncoder*>(p)->writer;
          return writer.key(field == provider::OriginalField::kName
                                ? "name"
                                : "short_name") &&
                 writer.beginString();
        },
        [](std::string_view bytes, void* p) {
          return static_cast<OriginalEncoder*>(p)->writer.stringChunk(bytes);
        },
        [](provider::OriginalField, void* p) {
          return static_cast<OriginalEncoder*>(p)->writer.endString();
        },
        [](void* p) { return static_cast<OriginalEncoder*>(p)->writer.end(); }};
  }
  bool finish() {
    return begin() && writer.end() && writer.end() && writer.finish() &&
           flush();
  }
};
// Uses the same interactive slot, scheduler and first-send budget gate as the
// selector. Backpressure/cancellation belongs to the bounded channel; no HTTP
// request, provider body or original label is retained in this worker.
bool stepOriginalNames(const settings::ProviderSettings& model,
                       uint32_t generation, uint32_t settings_generation,
                       int64_t& wake_ms) {
  const uint32_t query = original_query.load();
  if (!query) return false;
  if (!original_run.claimed) {
    if (!originals::claim(query, original_run.source)) {
      endOriginalNames(query, ESP_ERR_INVALID_STATE);
      return true;
    }
    original_run.claimed = true;
    provider::SelectionPlan preparation;
    const auto& source = original_run.source;
    if (!provider::prepareSelection(
            {source.provider, source.competition.view(), source.season.view()},
            provider::Operation::kOriginalNames, 0, preparation) ||
        !original_run.source.season.assign(preparation.season.data)) {
      endOriginalNames(query, ESP_ERR_INVALID_ARG);
      return true;
    }
    original_run.season_ready = !preparation.discover;
    run.generation = original_generation;
    run.epoch = original_epoch;
  }
  if (!originals::active(query) || generation != original_generation ||
      settings_generation != original_settings_generation) {
    endOriginalNames(query, ESP_ERR_INVALID_STATE);
    return true;
  }
  const auto now_ms = timekeeping::monotonicMs();
  const auto now_s = static_cast<uint32_t>(now_ms / 1000);
  const auto at = scheduler.admissibleAt(original_run.source.provider, now_s);
  if (at > now_s) {
    wake_ms = at * 1000LL;
    return false;
  }
  auto& source = original_run.source;
  const provider::RequestSpec request{
      {source.provider, source.competition.view(), source.season.view()},
      original_run.season_ready ? provider::Operation::kOriginalNames
                                : provider::Operation::kCurrentSeason,
      0,
      0,
      0,
      0,
      0,
      {}};
  OriginalEncoder encoder(query, source);
  auto output = encoder.sink();
  provider::Sink sink;
  sink.context = &encoder;
  sink.cancelled = [](void* p) {
    return !originals::active(static_cast<OriginalEncoder*>(p)->query);
  };
  if (original_run.season_ready) sink.originals = &output;
  provider::ParseResult parsed;
  busy.store(true);
  const auto err =
      get(request, model, run.generation, original_settings_generation,
          run.epoch, sink, parsed, HttpPurpose::kSetup);
  busy.store(false);
  if (fetchOf(err) == plan::Fetch::kBusy && originals::active(query)) {
    wake_ms = now_ms + cfg::kRequestGapMs;
    return false;
  }
  if (err == ESP_OK && !original_run.season_ready) {
    if (parsed.metadata.season.empty() ||
        !source.season.assign(parsed.metadata.season.data)) {
      endOriginalNames(query, ESP_ERR_INVALID_RESPONSE);
      return true;
    }
    original_run.season_ready = true;
    return true;
  }
  endOriginalNames(query, err == ESP_OK && !encoder.finish() ? ESP_FAIL : err);
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
bool stepDiscovery(const settings::ProviderSettings& model, uint32_t generation,
                   uint32_t settings_generation, uint32_t utc) {
  // A reboot restarts the RAM traversal against current inputs, using only
  // whatever durable credits remain. A changed source within this boot
  // retires the run before any request against the replacement source.
  if (!discovery_binding) discovery_binding = coverage_binding;
  discovery_retire |=
      discovery.stored().active && discovery_binding != coverage_binding;
  if (discovery_retire) {
    if (!finishDiscovery()) return false;
    discovery_retire = false;
  }
  if (!eligibility.selection.automatic_break || eligibility.frame || utc == 0 ||
      routes.entries.empty())
    return false;
  const auto today = utc / timekeeping::kSecondsPerDay;
  const auto& stored = discovery.stored();
  if (!stored.active && stored.runs.day != 0 && today <= stored.runs.day)
    return false;
  // Traverse active, usable sources that support interval proof. Display
  // rows alone never prove a gap; accepted proof needs a complete response.
  while (discovery_progress.route < route_count) {
    const auto route = discovery_progress.route;
    const auto& entry = routes.entries[route];
    if (!entry.active || entry.primary.competition.empty() ||
        !settings::providerUsable(entry.primary.provider, model) ||
        !provider::capabilities(entry.primary.provider).coverage) {
      ++discovery_progress.route;
      discovery_progress.season = 0;
      continue;
    }
    const auto now_s = static_cast<uint32_t>(timekeeping::monotonicMs() / 1000);
    if (scheduler.admissibleAt(entry.primary.provider, now_s) > now_s)
      return false;
    // Reserve the same operation before allocating discovery state, and
    // retain it through proof acceptance and release of that state.
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
    provider::Sink sink;
    sink.context = response;
    sink.fixture = [](const provider::Fixture& row,
                      std::span<const provider::Event>, void* raw) {
      static_cast<coverage::Response*>(raw)->observe(row);
      return true;
    };
    provider::ParseResult parsed;
    const auto err = get(query->http.spec, model, generation,
                         settings_generation, network::status().epoch, sink,
                         parsed, HttpPurpose::kDiscovery, entry.id, &admission);
    response->parsed(parsed);
    busy.store(false);
    if (!sent) {
      // DNS/TLS alone do not spend a discovery request. A reserved run that
      // reached its fixed cap closes instead of retrying indefinitely.
      delete response;
      delete query;
      if (failure_cause == Cause::kCounter && discovery.stored().active)
        finishDiscovery();
      return false;
    }
    const auto clock = nowClock();
    const auto& span = response->finish(err == ESP_OK, clock.utc);
    lock();
    if (auto* proof = coverageOf(entry.id)) {
      coverage::accept(*proof, response->source(), span);
    }
    coverage_streaming = false;
    unlock();
    delete response;
    delete query;
    if (++discovery_progress.season >= cfg::kCoverageMaxSeasons) {
      discovery_progress.season = 0;
      ++discovery_progress.route;
    }
    if (discovery_progress.route >= route_count) finishDiscovery();
    lock();
    publish();
    unlock();
    events::post(events::Event::kDataUpdated);
    return true;
  }
  finishDiscovery();
  return false;
}
// Fill the worker's existing storage from one locked generation. Separately
// reading model/routes/counters could mix a concurrent competition reorder
// with another snapshot's positions and identities.
void reloadLocked(settings::ProviderSettings& model,
                  uint32_t& settings_generation, uint32_t& data_generation) {
  settings::footballSettings(
      model, routes, settings_generation, data_generation, &eligibility,
      [](const settings::ProviderSettings& snapshot,
         settings::CompetitionRoutes& effective, bool saved_competitions) {
        if (snapshot.demo)
          demo_bindings =
              demo::configure(effective, demo_scenario, saved_competitions);
        else
          demo_bindings = {};
      });
  coverage_binding = coverageBinding(model);
}
void reload(settings::ProviderSettings& model, uint32_t& settings_generation,
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
  settings::ProviderSettings model;
  Access access{};
  for (;;) {
    WorkerActivity activity;
    if (!activity) {
      vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
      continue;
    }
    network::HeavyGuard admission;
    if (!admission) {
      vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
      continue;
    }
    loadDiscovery();
    reload(model, settings_seen, applied);
    access = accessOf(model);
    applyRoutes(model, applied, true);
    break;
  }
  int64_t maintained_ms = 0;
  bool automatic_turn = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
    WorkerActivity activity;
    if (!activity) continue;
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
      } else {
        // Club and view preferences do not change the provider dataset.
        // Refresh their projection over retained facts, without resetting
        // jobs, completeness evidence or the last successful request.
        const auto clock = nowClock();
        for (const auto& entry : routes.entries)
          if (entry.active)
            store.selectClub(entry.id, entry.primary.provider, entry.club.team,
                             clock);
        publish();
      }
      unlock();
      routesApplied();
      // Changed request plans and limits apply with the next decision.
    }
    // A clock step re-estimates at once. Neither a clock step nor a new
    // network epoch fetches again: overdue jobs run anyway.
    if (time_changed.exchange(false)) maintained_ms = 0;
    time_t utc = 0;
    const bool clock_valid = timekeeping::utcNow(utc);
    if (ota::trial()) {
      if (const auto query = original_query.load())
        endOriginalNames(query, ESP_ERR_INVALID_STATE);
      setCadence(Cadence::kUpdateTest, 0);
      continue;
    }
    if ((!network::ready() || !clock_valid) && !model.demo) {
      // Under the mutex, like select(), so a new request cannot be lost.
      lock();
      cadence_now = Cadence::kWaiting;
      due_ms = 0;
      if (const auto query = original_query.load()) {
        originals::complete(query, ESP_ERR_INVALID_STATE);
        original_query.store(0);
        original_run = {};
      }
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
    day = static_cast<uint32_t>(utc / timekeeping::kSecondsPerDay);
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
    // Configure wizard limits even when automatic work is paused. Decide
    // automatic eligibility before choosing the lane: a continuous stream of
    // interactive requests must leave every second actual send to a due job.
    scheduler.configure(model.providers, model.global_limit);
    schedule::Decision decision{};
    if (!model.demo && !settings::footballPaused()) {
      if (requested.exchange(false)) scheduler.refresh();
      const auto decision_clock = nowClock();
      const auto decision_dates = localDates(utc);
      const bool break_proven =
          !coverage_streaming && coverage::assess(coverage_evidence, routes,
                                                  utc, eligibility.break_days)
                                         .verdict == coverage::Verdict::kBreak;
      const selection::Input selection_input{
          eligibility.selection, routes,         store,
          decision_clock,        decision_dates, break_proven};
      const auto selected = selection::select(selection_input);
      planner.context(routes, eligibility, decision_dates, break_proven);
      for (uint8_t i = 0; i < cfg::kCompetitionMax; ++i)
        planner.wanted(
            i, i < routes.entries.size() ? &routes.entries[i] : nullptr, store,
            decision_clock, inputs.competitions[i], &selected);
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
      decision = scheduler.next(inputs);
    }
    int64_t selection_wake = INT64_MAX;
    if (selecting.load() && (!automatic_turn || !decision.run)) {
      sent = false;
      if (original_query.load())
        stepOriginalNames(model, generation, settings_seen, selection_wake);
      else
        stepSelection(model, generation, settings_seen, utc, selection_wake);
      // Terminal steps and local refusals have made no provider request;
      // they do not spend the automatic job's turn.
      if (sent) {
        automatic_turn = true;
        continue;
      }
    }
    if (model.demo) {
      const auto command = demo_command.load();
      if (command != kNoDemoCommand) {
        lock();
        const bool changed =
            demo_scenario != static_cast<demo::Scenario>(command >> 8);
        demo_scenario = static_cast<demo::Scenario>(command >> 8);
        demo_step = static_cast<uint8_t>(command);
        ESP_LOGI("provider", "demo scenario %u step %u",
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
      lock();
      cadence_now = Cadence::kNone;
      due_ms = retry_ms = 0;
      unlock();
      continue;
    }
    if (decision.run) {
      sent = false;
      runJob(decision.job, model, generation, settings_seen,
             network::status().epoch);
      // A refused automatic attempt keeps its scheduler retry, but must not
      // win every device-gap boundary ahead of the interactive owner.
      automatic_turn = false;
      continue;
    }
    if (stepDiscovery(model, generation, settings_seen, utc)) continue;
    const int64_t due = decision.wake_s ? decision.wake_s * 1000LL : 0;
    const auto cadence = cadenceOf(decision, utc, inputs.clock.mono_s);
    lock();
    cadence_now = cadence;
    due_ms = due;
    retry_ms = error_routes ? due : 0;
    unlock();
    // Sleeps until the wake time (at most until the next maintenance) or
    // until something asks for a new decision.
    const auto original = original_query.load();
    const auto original_deadline =
        original ? static_cast<int64_t>(originals::deadlineMs(original))
                 : INT64_MAX;
    const int64_t wake =
        std::min({due ? due : INT64_MAX, selection_wake, original_deadline,
                  selectionDeadline(), now + cfg::kStoreMaintenanceS * 1000LL});
    const uint32_t epoch = network::status().epoch;
    activity.release();
    while (
        timekeeping::monotonicMs() < wake && !requested.load() &&
        (!selecting.load() || run.active || original_run.claimed) &&
        (!selecting.load() || original_query.load() ||
         selectionAlive(run.query)) &&
        (!original_query.load() || originals::active(original_query.load())) &&
        !time_changed.load() && settings::generation() == settings_seen &&
        settings::dataGeneration() == generation && network::ready() &&
        network::status().epoch == epoch && !ota::trial())
      vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
  }
}
}  // namespace
esp_err_t init() {
  if (!selection_boot_nonce) {
    selection_boot_nonce = esp_random();
    if (!selection_boot_nonce) selection_boot_nonce = 1;
  }
  mutex = xSemaphoreCreateMutexStatic(&mutex_buffer);
  const auto budget_error = budget::init();
  if (budget_error != ESP_OK)
    ESP_LOGE("provider", "request counters unreadable: %s",
             esp_err_to_name(budget_error));
  const auto subscription = events::subscribe(
      events::Event::kTimeChanged,
      [](events::Event, void*) { time_changed.store(true); }, nullptr);
  if (subscription != ESP_OK) return subscription;
  return xTaskCreateStatic(worker, "provider", cfg::kProviderStackBytes,
                           nullptr, cfg::kProviderTaskPriority, stack,
                           &task_buffer)
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
  s.matches = static_cast<uint16_t>(store.matchCount());
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
  if (reset_fence) {
    unlock();
    return;
  }
  const bool stale = markStale(states, valid ? utc : 0, now);
  uint8_t loaded_matches = 0, loaded_tables = 0;
  for (uint8_t i = 0; i < route_count; ++i) {
    if (store.datasetStatus(slots[i].id, MatchStore::Dataset::kFixtures)
            .last_success_ms ||
        demo_mode)
      loaded_matches |= 1u << i;
    if (store.datasetStatus(slots[i].id, MatchStore::Dataset::kTable)
            .last_success_ms ||
        demo_mode)
      loaded_tables |= 1u << i;
  }
  const Published data{
      store,
      shown,
      revision,
      published_generation,
      configured,
      stale,
      oldestFetch(),
      retry_ms,
      states,
      routes,
      demo_mode ? demo::breakProven(demo_scenario, eligibility.break_days)
                : !coverage_streaming && valid &&
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
esp_err_t start(const SelectionRequest& request, SelectionQuery& query) {
  query = 0;
  if (!mutex || ota::trial()) return ESP_ERR_INVALID_STATE;
  const auto generation = settings::generation();
  lock();
  // The complete request and its lease exist before the worker sees the flag.
  if (reset_fence || selecting.load()) {
    unlock();
    return ESP_ERR_NOT_FINISHED;
  }
  if (generation != settings::generation() || !selection_boot_nonce ||
      selection_counter == UINT32_MAX) {
    unlock();
    return ESP_ERR_INVALID_STATE;
  }
  selection_request = request;
  selection_state.query =
      (SelectionQuery{selection_boot_nonce} << 32) | ++selection_counter;
  selection_settings_generation = generation;
  selection_deadline_ms =
      timekeeping::monotonicMs() + cfg::kOperationDeadlineMs;
  selection_cancelled = false;
  selection_state.busy = true;
  selection_state.count = 0;
  selection_state.more = false;
  selection_state.teams = request.teams;
  selection_state.cause = Cause::kNone;
  selection_state.provider = request.provider;
  selection_state.http = 0;
  selection_state.wait = Cadence::kNone;
  selection_state.wait_ms = 0;
  query = selection_state.query;
  selecting.store(true);
  unlock();
  return ESP_OK;
}
}  // namespace
esp_err_t select(const SelectionRequest& request, SelectionQuery& query) {
  return start(request, query);
}
esp_err_t beginOriginalNames(uint8_t competition, uint32_t& query) {
  query = 0;
  if (!mutex || ota::trial()) return ESP_ERR_INVALID_STATE;
  settings::Competition entry;
  const auto before = settings::generation();
  if (!settings::competitionById(competition, entry) ||
      !entry.primary.complete())
    return ESP_ERR_NOT_FOUND;
  lock();
  if (reset_fence || selecting.load() || settings::generation() != before) {
    unlock();
    return ESP_ERR_NOT_FINISHED;
  }
  const auto err = originals::begin(entry.primary, query);
  if (err == ESP_OK) {
    original_generation = settings::dataGeneration();
    original_settings_generation = before;
    original_epoch = network::status().epoch;
    original_query.store(query);
    selecting.store(true);
  }
  unlock();
  return err;
}

esp_err_t selectionStatus(SelectionQuery query, SelectionStatus& out) {
  if (!mutex) return ESP_ERR_NOT_FOUND;
  lock();
  const auto now = timekeeping::monotonicMs();
  if (!selectionCurrentLocked(query, now)) {
    unlock();
    return ESP_ERR_NOT_FOUND;
  }
  // Only the matching owner's read renews the idle lease. Expired, cancelled
  // and foreign IDs cannot revive or prolong an abandoned operation.
  selection_deadline_ms = now + cfg::kOperationDeadlineMs;
  out = selection_state;
  unlock();
  return ESP_OK;
}
namespace {
template <class Row>
bool readSelectionRow(SelectionQuery query, uint16_t index, uint32_t revision,
                      Row& out) {
  if (!mutex) return false;
  lock();
  const bool ok = selectionCurrentLocked(query, timekeeping::monotonicMs()) &&
                  !selection_state.busy &&
                  selection_state.teams == std::is_same_v<Row, TeamSelection> &&
                  revision == selection_state.revision &&
                  index < selection_state.count;
  if (ok) selections.get(index, out);
  unlock();
  return ok;
}
}  // namespace
bool selectionAt(SelectionQuery query, uint16_t index, uint32_t revision,
                 CompetitionSelection& out) {
  return readSelectionRow(query, index, revision, out);
}
bool selectionAt(SelectionQuery query, uint16_t index, uint32_t revision,
                 TeamSelection& out) {
  return readSelectionRow(query, index, revision, out);
}

esp_err_t cancelSelection(SelectionQuery query) {
  if (!mutex) return ESP_ERR_NOT_FOUND;
  lock();
  if (!selectionCurrentLocked(query, timekeeping::monotonicMs())) {
    unlock();
    return ESP_ERR_NOT_FOUND;
  }
  selection_cancelled = true;
  // The worker alone releases an active lease after the synchronous provider
  // request has stopped. Starting another query before that would reuse its
  // borrowed request/sink storage.
  unlock();
  return ESP_OK;
}
Overview overview(int64_t utc, Status* status_out,
                  DatasetSnapshots* datasets_out) {
  Overview result{};
  if (status_out != nullptr) status_out->started = false;
  if (mutex == nullptr) return result;
  const int64_t now = status_out != nullptr ? timekeeping::monotonicMs() : 0;
  lock();
  if (reset_fence) {
    unlock();
    return result;
  }
  if (status_out != nullptr) statusLocked(*status_out, utc, now);
  if (datasets_out) {
    *datasets_out = {};
    for (uint8_t i = 0; i < route_count; ++i)
      for (uint8_t dataset = 0; dataset < 3; ++dataset)
        (*datasets_out)[i][dataset] = store.datasetStatus(
            slots[i].id, static_cast<MatchStore::Dataset>(dataset));
  }
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
    info.matches = view::matchCount(store, shown, slot.id);
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
    if (m.home < cfg::kStoreTeams) {
      own.home_id = store.teamAt(m.home).id;
      own.provider = store.teamAt(m.home).provider;
    }
    if (m.away < cfg::kStoreTeams) own.away_id = store.teamAt(m.away).id;
    own.kickoff = m.kickoff;
    if (m.principal.valid()) {
      own.home_goals = m.principal.home;
      own.away_goals = m.principal.away;
    }
    if (m.minute != kNoValue) own.minute = m.minute;
    Text<cfg::kTeamNameBytes> label;
    label.assign(view::teamName(store, m, true));
    own.home.assign(label.data);
    label.assign(view::teamName(store, m, false));
    own.away.assign(label.data);
  }
  unlock();
  return result;
}
namespace {
// Under the mutex: store team indices the matches and rows of
// 'competition' refer to.
std::bitset<cfg::kStoreTeams> teamsOf(uint8_t competition) {
  std::bitset<cfg::kStoreTeams> used;
  const auto mark = [&used](TeamIndex team) {
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
  if (reset_fence) {
    unlock();
    return 0;
  }
  revision_out = revision;
  const auto count = static_cast<uint16_t>(teamsOf(competition).count());
  unlock();
  return count;
}
bool teamAt(uint8_t competition, uint16_t index, uint32_t revision_in,
            TeamInfo& out) {
  bool found = false;
  out = {};
  lock();
  if (!reset_fence && revision_in == revision) {
    const auto used = teamsOf(competition);
    for (std::size_t i = 0, seen = 0; i < used.size() && !found; ++i) {
      if (!used.test(i) || seen++ != index) continue;
      const auto& team = store.teamAt(static_cast<TeamIndex>(i));
      out.id = team.id;
      out.provider = team.provider;
      out.name.assign(team.name.data);
      out.short_name.assign(team.short_name.data);
      found = true;
    }
  }
  unlock();
  return found;
}
esp_err_t beginReset() {
  if (mutex == nullptr || ota::trial()) return ESP_ERR_INVALID_STATE;
  lock();
  const bool busy = reset_fence || worker_active;
  if (!busy) reset_fence = true;
  unlock();
  return busy ? ESP_ERR_TIMEOUT : ESP_OK;
}
void endReset() {
  if (mutex == nullptr) return;
  lock();
  reset_fence = false;
  unlock();
}
}  // namespace football
