#include "time_service.h"

#include <cstdlib>

#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "time_service_internal.h"

namespace timekeeping {

namespace {

constexpr const char* kTag = "time";

// Serialises the zone (TZ and the C library's parsed rule, which
// localtime_r reads), the validity state and every conversion.
StaticSemaphore_t mutex_buffer;
SemaphoreHandle_t mutex = nullptr;

class Lock {
 public:
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

// Entry of cfg::kTimeZones in effect; nullptr until one was applied (the C
// library then uses UTC).
const cfg::TimeZone* applied_zone = nullptr;
const cfg::TimeZone* requested_zone = nullptr;
Status state{false, Source::kNone, 0, 0, 0};
// The settings' zone could not be applied; poll() retries at this time.
// App task only, like configureZone().
bool zone_pending = false;
int64_t zone_retry_at = 0;

enum class ZoneResult : uint8_t { kUnchanged, kChanged, kFailed };

ZoneResult applyZoneLocked(const cfg::TimeZone& zone) {
  if (&zone == applied_zone) return ZoneResult::kUnchanged;
  // setenv copies the rule to the heap; on failure TZ keeps its old value.
  if (setenv("TZ", zone.rule, 1) != 0) return ZoneResult::kFailed;
  tzset();
  applied_zone = &zone;
  ++state.step_count;
  return ZoneResult::kChanged;
}

const char* appliedLabelLocked() {
  return applied_zone != nullptr ? applied_zone->label : "none (UTC)";
}

void logLocal(const char* prefix, const std::tm& local, const char* abbr) {
  ESP_LOGI(kTag, "%s%04d-%02d-%02d %02d:%02d:%02d %s", prefix,
           local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
           local.tm_min, local.tm_sec, abbr);
}

}  // namespace

const char* sourceName(Source source) {
  switch (source) {
    case Source::kNone: return "none";
    case Source::kSntp: return "SNTP";
    case Source::kDeviceTest: return "device test";
  }
  return "?";
}

void init(const cfg::TimeZone& zone) {
  mutex = xSemaphoreCreateMutexStatic(&mutex_buffer);
  configureZone(zone);
  ESP_LOGI(kTag, "wall clock not valid until set (SNTP from P3.1)");
}

void configureZone(const cfg::TimeZone& desired) {
  requested_zone = &desired;
  const auto* zone = requested_zone;
  ZoneResult result;
  {
    Lock lock;
    result = applyZoneLocked(*zone);
  }
  zone_pending = result == ZoneResult::kFailed;
  switch (result) {
    case ZoneResult::kUnchanged: return;
    case ZoneResult::kFailed:
      zone_retry_at = monotonicMs() + cfg::kTimeZoneRetryMs;
      ESP_LOGE(kTag,
               "zone %s not applied (no memory), previous zone stays; retry "
               "in %lu s",
               zone->label,
               static_cast<unsigned long>(cfg::kTimeZoneRetryMs / 1000));
      return;
    case ZoneResult::kChanged:
      ESP_LOGI(kTag, "zone %s (%s)", zone->label, zone->rule);
      events::post(events::Event::kTimeChanged);
      return;
  }
}

void poll() {
  if (zone_pending && monotonicMs() >= zone_retry_at)
    configureZone(*requested_zone);
}

esp_err_t setTime(const timeval& utc, Source source) {
  bool first = false;
  int64_t jump_s = 0;
  {
    Lock lock;
    timeval before{};
    gettimeofday(&before, nullptr);
    if (settimeofday(&utc, nullptr) != 0) {
      ESP_LOGE(kTag, "clock refused %lld (%s)",
               static_cast<long long>(utc.tv_sec), sourceName(source));
      return ESP_FAIL;
    }
    first = !state.valid;
    jump_s = static_cast<int64_t>(utc.tv_sec) - before.tv_sec;
    if (first || llabs(jump_s) > cfg::kTimeJumpThresholdS) ++state.step_count;
    state.valid = true;
    state.source = source;
    state.set_at_ms = monotonicMs();
    ++state.set_count;
  }
  if (first) {
    std::tm local{};
    toLocal(utc.tv_sec, local);
    logLocal("time valid: ", local, sourceName(source));
  } else if (llabs(jump_s) > cfg::kTimeJumpThresholdS) {
    ESP_LOGW(kTag, "clock jumped by %lld s (%s)",
             static_cast<long long>(jump_s), sourceName(source));
  }
  events::post(events::Event::kTimeChanged);
  return ESP_OK;
}

Status status() {
  Lock lock;
  return state;
}

bool utcNow(time_t& out) {
  Lock lock;
  if (!state.valid) return false;
  out = time(nullptr);
  return true;
}

bool localNow(std::tm& out) {
  Lock lock;
  if (!state.valid) return false;
  const time_t now = time(nullptr);
  localtime_r(&now, &out);
  return true;
}

void toLocal(time_t utc, std::tm& out) {
  Lock lock;
  localtime_r(&utc, &out);
}

int32_t localOffset(time_t utc) {
  std::tm local{};
  toLocal(utc, local);
  const int64_t days = daysFromCivil(local.tm_year + 1900,
                                     static_cast<unsigned>(local.tm_mon + 1),
                                     static_cast<unsigned>(local.tm_mday));
  const int64_t as_utc =
      days * 86400 + local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
  return static_cast<int32_t>(as_utc - utc);
}

int64_t monotonicMs() { return esp_timer_get_time() / 1000; }

void logStatus() {
  Lock lock;
  if (!state.valid) {
    ESP_LOGI(kTag, "not valid, zone %s", appliedLabelLocked());
    return;
  }
  const time_t now = time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  logLocal("", local, tzname[local.tm_isdst > 0 ? 1 : 0]);
  ESP_LOGI(kTag, "zone %s, set by %s %lld s ago, %u sets", appliedLabelLocked(),
           sourceName(state.source),
           static_cast<long long>((monotonicMs() - state.set_at_ms) / 1000),
           static_cast<unsigned>(state.set_count));
}

namespace internal {

void restoreRequestedZone() { configureZone(*requested_zone); }

bool applyZone(const cfg::TimeZone& zone) {
  ZoneResult result;
  {
    Lock lock;
    result = applyZoneLocked(zone);
  }
  if (result == ZoneResult::kChanged) {
    events::post(events::Event::kTimeChanged);
  }
  return result != ZoneResult::kFailed;
}

}  // namespace internal

}  // namespace timekeeping
