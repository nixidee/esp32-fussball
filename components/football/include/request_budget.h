#pragma once
#include "app_config.h"
#include "esp_err.h"
namespace football::budget {
// Single provider-worker owner. Durable reservations prevent rebooting from
// replenishing a daily allowance. Unused reserved credits are lost at boot.
esp_err_t init();
esp_err_t admit(cfg::Provider provider, uint32_t utc_day, uint32_t limit);
uint32_t remaining(cfg::Provider provider, uint32_t limit);
}  // namespace football::budget
