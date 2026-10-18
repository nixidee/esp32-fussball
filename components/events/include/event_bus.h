// State notifications through one esp_event loop/task. Each kind coalesces;
// subscribers read current owner state. UI commands go directly to ui::submit.
// Posting never blocks and carries no stale data or heap payload.

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "event_admission.h"

namespace events {

// Runs in the bus task. Keep it short: set a flag or notify the subscriber's
// own task; never block, never call LVGL.
using Callback = void (*)(Event event, void* context);

// Creates the loop and its task. Call once at boot before any other service
// posts. Errors come from esp_event (no memory for the loop or task).
esp_err_t init();

// Adds a callback for one event; any task, any time. A callback added after
// a post may miss that post, so read the current state after subscribing.
// ESP_ERR_NO_MEM: the table (cfg::kEventMaxSubscribers) is full.
esp_err_t subscribe(Event event, Callback callback, void* context);

// Task context only, not from an ISR. ESP_OK: queued/coalesced;
// ESP_ERR_INVALID_ARG: invalid kind; ESP_ERR_INVALID_STATE: no loop.
// Other esp_event failures are counted. Every valid kind has one queue slot.
esp_err_t post(Event event);

// Bus task stack low-water mark and failed posts.
void logStatus();

// Runs the device test selected by cfg::kEventTest (app_config.h); does
// nothing in a normal build. Call after init().
void runDeviceTest();

}  // namespace events
