#include "request_budget.h"

#include "budget_ledger.h"
#include "esp_log.h"
#include "nvs.h"

namespace football::budget {
namespace {
constexpr unsigned kApiFootball =
    static_cast<unsigned>(cfg::Provider::kApiFootball);
// One counter per provider, then the global one.
constexpr unsigned kGlobal = cfg::kProviderCount;
Counter counters[cfg::kProviderCount + 1];

const char* key(unsigned index) {
  return index == kGlobal ? cfg::kBudgetGlobalNvsKey : cfg::kBudgetNvsKeys[index];
}
// The record of earlier firmware is still stored (it is read only while
// API-Football has no record of its own).
bool legacy_stored = false;

// Reads one blob; 'buffer' receives at most 'size' bytes.
Load read(nvs_handle_t handle, const char* key, void* buffer, std::size_t size,
          std::size_t* length) {
  *length = size;
  const auto err = nvs_get_blob(handle, key, buffer, length);
  if (err == ESP_ERR_NVS_NOT_FOUND) return Load::kMissing;
  return err == ESP_OK ? Load::kValid : Load::kDamaged;
}

esp_err_t store(unsigned index, const Record& next) {
  nvs_handle_t handle;
  auto err = nvs_open(cfg::kBudgetNvsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(handle, key(index), &next, sizeof(next));
  if (err == ESP_OK && index == kApiFootball && legacy_stored) {
    const auto erased = nvs_erase_key(handle, cfg::kBudgetLegacyNvsKey);
    if (erased == ESP_OK || erased == ESP_ERR_NVS_NOT_FOUND)
      legacy_stored = false;
  }
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}
}  // namespace

esp_err_t init() {
  nvs_handle_t handle;
  auto err = nvs_open(cfg::kBudgetNvsNamespace, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;  // nothing stored yet
  // Without a readable namespace every counter starts empty; storing the
  // first reservation then reports the error for its provider only.
  if (err != ESP_OK) return err;
  for (unsigned i = 0; i <= kGlobal; ++i) {
    Record stored{};
    std::size_t length = 0;
    auto state = read(handle, key(i), &stored, sizeof(stored), &length);
    Record record{};
    // A missing record and a failed read are different: only a blob that
    // exists but cannot be read or checked is damaged.
    if (state == Load::kValid) state = parse(&stored, length, &record);
    if (state == Load::kMissing && i == kApiFootball) {
      LegacyRecord legacy{};
      state = read(handle, cfg::kBudgetLegacyNvsKey, &legacy, sizeof(legacy),
                   &length);
      legacy_stored = state != Load::kMissing;
      if (state == Load::kValid)
        state = parseLegacy(&legacy, length, i, &record);
    }
    counters[i].load(state, record);
    if (state == Load::kDamaged)
      ESP_LOGE("provider",
               "request counter %s unreadable: used up for today, new "
               "counter tomorrow",
               key(i));
  }
  nvs_close(handle);
  return ESP_OK;
}

static esp_err_t admitAt(unsigned index, uint32_t day, uint32_t limit) {
  if (limit == 0) return ESP_OK;
  Record next{};
  switch (counters[index].admit(day, limit, &next)) {
    case Counter::Verdict::kAllowed: return ESP_OK;
    case Counter::Verdict::kUsedUp: return ESP_ERR_NOT_ALLOWED;
    case Counter::Verdict::kStore: break;
  }
  const auto err = store(index, next);
  if (err == ESP_OK) counters[index].commit(next);
  return err;
}

esp_err_t admit(cfg::Provider provider, uint32_t day, uint32_t limit) {
  const auto index = static_cast<unsigned>(provider);
  if (index >= cfg::kProviderCount) return ESP_ERR_INVALID_ARG;
  return admitAt(index, day, limit);
}

esp_err_t admitGlobal(uint32_t day, uint32_t limit) {
  return admitAt(kGlobal, day, limit);
}

uint32_t remaining(cfg::Provider provider, uint32_t day, uint32_t limit) {
  const auto index = static_cast<unsigned>(provider);
  return index < cfg::kProviderCount ? counters[index].remaining(day, limit)
                                     : 0;
}

uint32_t remainingGlobal(uint32_t day, uint32_t limit) {
  return counters[kGlobal].remaining(day, limit);
}

esp_err_t erase() {
  nvs_handle_t handle;
  auto err = nvs_open(cfg::kBudgetNvsNamespace, NVS_READWRITE, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;
  err = nvs_erase_all(handle);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}
}  // namespace football::budget
