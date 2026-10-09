// Event bus (docs/ARCHITECTURE.md): typed notifications through an own
// esp_event loop and task. State events are coalesced (at most one queued per
// kind; subscribers read the current state from its owner). UI actions take
// one of cfg::kEventUiActionSlots queue slots and are dropped when all are
// taken. Posting never blocks and never allocates. Commands that need a
// result are direct calls, never events.

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "event_admission.h"

namespace events {

// Runs in the bus task. Keep it short: set a flag or notify the subscriber's
// own task; never block, never call LVGL.
using Callback = void (*)(Event event, uint32_t payload, void* context);

// Creates the loop and its task. Call once at boot before any other service
// posts. Errors come from esp_event (no memory for the loop or task).
esp_err_t init();

// Adds a callback for one event; any task, any time. A callback added after
// a post may miss that post, so read the current state after subscribing.
// ESP_ERR_NO_MEM: the table (cfg::kEventMaxSubscribers) is full.
esp_err_t subscribe(Event event, Callback callback, void* context);

// Task context only, not from an ISR. The payload is delivered with UI
// actions and ignored for state events.
// ESP_OK: queued, or a state event already queued (coalesced).
// ESP_ERR_TIMEOUT: UI action dropped, all UI slots taken.
// ESP_ERR_INVALID_STATE: init() has not succeeded.
// Other errors come from esp_event; the event is lost and counted.
esp_err_t post(Event event, uint32_t payload = 0);

// Bus task stack low-water mark, dropped UI actions and failed posts.
void logStatus();

// Runs the device test selected by cfg::kEventTest (app_config.h); does
// nothing in a normal build. Call after init().
void runDeviceTest();

}  // namespace events
