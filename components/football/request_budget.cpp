#include "request_budget.h"

#include <algorithm>

#include "nvs.h"

namespace football::budget {
namespace {
struct Ledger {
  uint32_t magic, day, reserved[cfg::kProviderCount];
};
Ledger ledger{};
uint32_t credits[cfg::kProviderCount]{};
bool ready = false;
esp_err_t store(const Ledger& next) {
  nvs_handle_t handle;
  auto err = nvs_open(cfg::kBudgetNvsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(handle, cfg::kBudgetNvsKey, &next, sizeof(next));
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}
}  // namespace
esp_err_t init() {
  nvs_handle_t handle;
  auto err = nvs_open(cfg::kBudgetNvsNamespace, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ready = true;
    return ESP_OK;
  }
  if (err != ESP_OK) return err;
  std::size_t length = sizeof(ledger);
  err = nvs_get_blob(handle, cfg::kBudgetNvsKey, &ledger, &length);
  nvs_close(handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ledger = {};
    ready = true;
    return ESP_OK;
  }
  if (err != ESP_OK || length != sizeof(ledger) ||
      ledger.magic != cfg::kBudgetRecordMagic)
    return ESP_ERR_INVALID_STATE;
  ready = true;
  return ESP_OK;
}
esp_err_t admit(cfg::Provider provider, uint32_t day, uint32_t limit) {
  if (!ready) return ESP_ERR_INVALID_STATE;
  const auto index = static_cast<unsigned>(provider);
  if (index >= cfg::kProviderCount) return ESP_ERR_INVALID_ARG;
  if (day > ledger.day) {
    Ledger next{};
    next.magic = cfg::kBudgetRecordMagic;
    next.day = day;
    auto err = store(next);
    if (err != ESP_OK) return err;
    ledger = next;
    for (auto& n : credits) n = 0;
  }
  // A backward clock correction cannot reset or move the durable day back.
  if (credits[index] && ledger.reserved[index] - credits[index] < limit) {
    --credits[index];
    return ESP_OK;
  }
  if (ledger.reserved[index] >= limit) return ESP_ERR_NOT_ALLOWED;
  const auto count = std::min<uint32_t>(cfg::kBudgetReservationRequests,
                                        limit - ledger.reserved[index]);
  auto next = ledger;
  next.reserved[index] += count;
  const auto err = store(next);
  if (err != ESP_OK) return err;
  ledger = next;
  credits[index] = count - 1;
  return ESP_OK;
}
uint32_t remaining(cfg::Provider provider, uint32_t limit) {
  const auto index = static_cast<unsigned>(provider);
  const auto used = ledger.reserved[index] - credits[index];
  return !ready || used >= limit ? 0 : limit - used;
}
}  // namespace football::budget
