#pragma once
#include "app_config.h"
#include "esp_err.h"
namespace football::budget {
// Single provider-worker owner (except erase()). One durable counter per
// provider with a daily cap (budget_ledger.h); a cap of 0 uses no counter
// and writes nothing. A damaged or unreadable counter affects only its own
// provider. Returns the first NVS error for the log; it never blocks a
// provider by itself.
esp_err_t init();
// ESP_OK, ESP_ERR_NOT_ALLOWED (the day's cap is reached) or the NVS error
// that kept the reservation from being stored.
esp_err_t admit(cfg::Provider provider, uint32_t utc_day, uint32_t limit);
// Requests left on UTC 'day' under 'limit' (> 0); a day without a request
// yet has the whole limit.
uint32_t remaining(cfg::Provider provider, uint32_t utc_day, uint32_t limit);
// The global daily cap over all providers (0 = none, nothing stored): one
// more durable counter of the same kind, admitted after the provider's.
esp_err_t admitGlobal(uint32_t utc_day, uint32_t limit);
uint32_t remainingGlobal(uint32_t utc_day, uint32_t limit);
// The first valid UTC day: every damaged counter counts as used up on it
// and starts afresh on the next day (without waiting for a request).
void settle(uint32_t utc_day);
// Settings reset: removes every stored counter (the device restarts next).
esp_err_t erase();
}  // namespace football::budget
