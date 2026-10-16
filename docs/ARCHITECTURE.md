# Architecture

> Status: the C6 implementation candidate contains core, network, all four
> providers, a fixed compact match store, device screens/input, images, embedded Web,
> live debug and OTA. The current C6 firmware/filesystem build passes; the new
> runtime services have no device/browser/maximum-load acceptance yet.
> Earlier core-service evidence below is historical and scoped accordingly.
> Current ownership and resource costs are described here; decision history
> remains in [DECISIONS.md](DECISIONS.md).

## Goals
- Show football data for one club on small displays (first: 240×240 round).
- Scale to other boards, displays (360/466 px, rectangular), touch and
  additional data providers **without rewriting** screens or logic.
- Robust 24/7 operation: no hangs, bounded memory, safe updates.

## Layers

```
 ┌────────────────────────────── Web UI (browser) ─────────────────────────────┐
 │ Overview · Competitions · Display · Images · Settings · System (gzip)     │
 └───────────────▲─────────────────────────────────────────────────────────────┘
                 │ REST /api/v1 (JSON), uploads
 ┌───────────────┴──────────┐   ┌───────────────────────────────────────────┐
 │ Web/API server            │   │ Network: WiFi manager (state machine),    │
 │ OTA, image upload, config │   │ SoftAP + captive DNS, mDNS, SNTP          │
 └───────┬──────────────────┘   └───────────────────────────────────────────┘
         │ settings / commands              │ network events
 ┌───────▼───────────────────────── Core ───▼──────────────────────────────────┐
 │ Settings store (schema from app_config.h, secrets bootstrap) · Event bus    │
 │ File service (LittleFS) · Time service · Logging/health                     │
 └───────┬──────────────────────────────────────────────────────▲──────────────┘
         │                                                      │ data events
 ┌───────▼─────────────── Data layer ──────────────────────────┴──────────────┐
 │ Providers (OpenLigaDB, API-Football, ESPN, …) → canonical model            │
 │ Match store (compact, change detection) · Request scheduler (due times,    │
 │ live windows, limits) · Request plans                                      │
 └───────┬────────────────────────────────────────────────────────────────────┘
         │ store views
 ┌───────▼─────────────── Presentation ───────────────────────────────────────┐
 │ Screen-mode resolver (pure) · Presenters → view models                     │
 │ Navigation controller ◄── UiActions ◄── Input mapper ◄── input drivers     │
 └───────┬────────────────────────────────────────────────────────────────────┘
         │ view models
 ┌───────▼─────────────── UI (LVGL) ──────────────────────────────────────────┐
 │ Views (render only) · Layout tokens / size classes · Overlay manager       │
 │ Display port (esp_lcd + LVGL) · SafeArea geometry                          │
 └───────┬────────────────────────────────────────────────────────────────────┘
         │
 ┌───────▼─────────────── Hardware profiles (compile time) ───────────────────┐
 │ boards/*.h · displays/*.h · targets/*.h (board + display + wiring)         │
 └────────────────────────────────────────────────────────────────────────────┘
```

## Responsibilities and rules
| Module | Does | Does not |
|---|---|---|
| Provider | HTTP request, parse, map to canonical model, report capabilities | Decide when to poll, know about screens |
| Match store | Hold matches, tables, teams and own-match events of all competitions, detect changes (score change, final whistle) | Network I/O |
| Request scheduler / plans | Choose the next job from due times and live windows, enforce the per-provider and global limits; turn a job into provider requests | Parsing |
| Screen-mode resolver | Pure function: (time, data state, settings, inputs available) → screen set + default | LVGL, I/O |
| Presenter | Build a view model for one screen from the store views + settings | Draw |
| View | Create LVGL objects from layout tokens, render view model, emit `UiAction` | Business logic, data access |
| Input mapper | Map raw events (button press/long, touch gesture) to `UiAction` by device config | Navigation decisions |
| Navigation controller | Apply `UiAction` to screen stack / scroll state | Input hardware |
| Overlay manager | Show/hide overlays by priority and timeout, place them inside the round area | Know overlay content logic |

## Threading and data ownership
### Current ownership

- The application loop polls input, health, network policy, screen presentation
  and OTA health every 20 ms. It builds a fixed view model and calls the view
  under `display::lock()`. LVGL's own worker renders under the same recursive
  lock. Views only render supplied data; provider and navigation logic stay
  outside the view.
- The UI controller owns one settings model and one view model, the input
  queue and the presentation cadence. Pure navigation policy owns screen,
  row/page and competition-cycle state; separate football and status
  presenters borrow those objects during a repository callback. They retain
  no repository pointers and allocate no additional model or view copy.
- HTTP image publication/deletion is the one explicit cross-task LVGL
  operation: it acquires that lock, drops a cache entry and swaps/removes a
  file after the old reader has finished. Validation and upload run outside
  the lock. No event callback calls LVGL.
- This keeps the existing app/LVGL tasks rather than adding a separate UI
  task. It replaces the earlier provisional exclusive-UI-task plan. Every
  mode reuses two persistent child objects (one image and one draw object)
  under the active LVGL screen. The fixed application draw list and text
  arena do not replace LVGL's temporary draw-task allocations.
- One provider worker owns all fetches, parsing, the match store and
  publication. The store (26,712 B) is one static object; the worker writes
  each response element into it under a mutex and publishes after every
  request. `football::read()` holds that mutex while the presenter builds
  its bounded owned view model. The worker also keeps each active
  competition's data in a LittleFS file and loads it at boot; file I/O
  holds the heavy-operation token. A provider request never waits for that
  token or for a limit: the worker asks the request scheduler what may run,
  reports each request's start and outcome to it, and sleeps until the
  scheduler's next wake time. The owner's selection list is a pending work
  item served one request per pass.
  Status readers copy only bounded status. A caller may not retain model
  references after its reader callback returns. Reader lock duration is
  still to be measured.
- Parser strings are copied into the store's fixed records before the
  element is released. Every request carries the provider-data generation of the
  settings and the network epoch; obsolete results cannot publish. Missing and estimated values retain
  explicit provenance.
- The HTTP worker performs strict settings mutations and bounded streaming
  uploads. Scan/selection/refresh requests signal their owning service.
  The static debug worker permits one fixed in-flight message. Heavy admission
  serializes provider TLS/JSON with image mutation and OTA.
- Settings live behind a mutex. `settings::current()` returns an 832-byte
  owned copy on the C6 (it lives on the caller's stack). Its optional
  generation output belongs to that same locked snapshot; model publication
  advances the generation before unlocking and posts the notification after
  unlocking. The competition
  store uses that same settings mutex; readers copy one entry (153 B) or the whole store
  (1,226 B) into storage they own. NVS saves/reset use a second mutex and execute in the
  caller task. They post coalesced `kSettingsChanged` notifications.
- State notifications and semantic input actions use the existing bounded
  event bus. UI's controller action queue also has four fixed entries and
  reports overflow. Direct commands return errors rather than disappearing.
- DNS name resolution uses one static token-protected job dispatched to the
  existing lwIP thread. DNS callbacks never borrow caller stack/TLS storage.

### Task model (ESP32-C6, single core)

These are allocated/configured candidate sizes, checked against source and
the generated C6 sdkconfig. The candidate has no measured stack high-water
marks. Previously measured boot/core marks below do not apply to the new load.

| Task | Priority | Allocated stack | Owner |
|---|---|---|---|
| `esp_timer` | 22 | 4,096 B | SDK timer callbacks |
| `sys_evt` | 20 | 2,816 B | SDK WiFi/IP event loop |
| `events` | 5 | 2,304 B | Application notifications |
| HTTP server | 5 | 10,240 B | REST/upload worker |
| `taskLVGL` | 4 | 7,168 B | Existing LVGL port |
| `provider` | 3 | 12,800 B, static | Scheduling, fetch, parser, store |
| `portal_dns` | 2 | 2,048 B, static | Captive DNS (512 B query buffer static, outside the stack) |
| `web_debug` | 2 | 4,096 B, static | Controlled live stream |
| `main` | 1 | 8,704 B | App/service loop |
| mDNS | 1 | 4,096 B | Managed component |
| `Tmr Svc` | 1 | 2,048 B | FreeRTOS timers |
| `IDLE` | 0 | 1,536 B | FreeRTOS |

SDK WiFi and TCP/IP workers also consume memory. TCP/IP is configured with
3,072 B plus the Picolibc 512 B allowance. Picolibc adds that same 512 B to
configured main/timer/default-event-loop values: 8,192/3,584/2,304 B.
Static worker stacks are already included in the linker static total and must
not be counted again as an additional heap charge. Dynamic stacks, SDK worker
control objects and queues remain runtime charges.

The earlier normal core image measured free stack main 2,480/4,096 B,
LVGL 3,428/7,168 B, events 1,940/2,304 B, timer 3,816/4,096 B, idle
1,300/1,536 B and timer-service 1,764/2,048 B. Only that historical load met
the 25% free-stack target. The current 16-task health snapshot cap must also
be exercised with all services running.

### Event bus (implemented, device-tested)
`components/core/event_bus.*`; the pure catalog and admission logic is
`components/events` (host-tested). Design decision: ADR-019.

- **Own loop:** a separate `esp_event` loop with its own task (`events`),
  not the ESP-IDF default loop. The default loop stays reserved for WiFi and
  IP events, so a slow subscriber here cannot delay the network stack.
- **State notifications:** an event says *that* something changed and
  carries no data. Its receiver fetches the current state from its owner
  (for example `settings::current()`). UI actions use an 8 B (`uint64_t`)
  payload containing the action and the full 32-bit settings generation.
  All eight bytes stay inside the SDK queue entry; no project-bus post
  allocates event-data heap.
- **Catalog** (`event_admission.h`): state events `kSettingsChanged`,
  `kNetworkState`, `kDataUpdated`, `kOtaState`, `kTimeChanged`; the action
  event `kUiAction` with the action code and its model version. Core, network, data,
  OTA and input services now use this catalog.
- **State events coalesce:** while one is queued, further posts of the same
  kind are absorbed (`post` returns `ESP_OK`). The pending mark is cleared
  just before the subscribers run, so a change made during a callback is
  queued again and never lost.
- **UI actions are bounded:** at most `kEventUiActionSlots` (4) are queued.
  A further post is dropped, counted and answered with `ESP_ERR_TIMEOUT`;
  the first drop of a burst logs one warning. Dropping a keypress under
  overload is preferred to a growing backlog of stale input.
- **Queue length** = number of state kinds + UI slots (9). A state event
  always finds room, even with all UI slots taken; a failed post can only
  come from a defect and is logged and counted.
- **Subscribers:** fixed table of `kEventMaxSubscribers` (8) entries, no
  unsubscribe; `subscribe` returns `ESP_ERR_NO_MEM` when it is full. All
  callbacks run one after another in the `events` task on its 2304 B stack:
  they must not block, must not call LVGL and must not do heavy work (hand
  that to the owning task instead).
- **Boot order:** `events::init()` runs before the settings store. A failure
  is a memory budget error at boot and restarts the device in a controlled
  way.
- **Memory:** before the health/FreeRTOS trace extension, static use was
  approximately 120 B (`.bss`) and the nine-entry queue's init used 2,824 B
  of heap on the C6. The earlier eight-entry queue used 2,808 B (2304 B task
  stack, 8 × 16 B entries, loop and handler records). Trace adds control-object
  storage as listed below; these earlier measurements are historical. The
  boot log line `after event bus init` measures the current init cost.
  The earlier flood test used at most 364 B of the `events` stack
  (1,940 B free); subscriber stack usage is checked when services are added.
  The 2026-10-16 full-generation action transport changes C6 SDK entries
  from 16 to 20 B. The nine-entry project queue requests 36 B more storage;
  the 32-entry default loop requests 128 B more because inline capacity is
  a shared SDK setting. Total queue payload growth is 164 B, plus 16 B
  static for the existing four-entry UI FIFO. These are ABI/storage-size
  calculations; current allocator charges and task reserves need device
  measurement. Queue lengths and tasks remain unchanged.
- **Diagnostics:** the status log prints the minimum free stack of the
  `events` task and the counters for dropped UI actions and failed posts.
- **Device test** (`cfg::kEventTest = EventTest::kFlood` in `app_config.h`,
  back to `kNone` afterwards): posts 20 UI actions to a subscriber that
  blocks 50 ms each, then 5 settings events. Expected:
  `PASS: UI actions 5 delivered, 15 dropped; settings events 5 posted,
  1 delivered; failed posts 0`. The test accepts every result within the
  bounds (at most slots + 1 delivered, at least one drop, coalesced settings
  events, no failed post). Passed on the XIAO ESP32-C6 with exactly this
  line (2026-10-09).

## File service (implemented, device-tested)
`components/core/file_service.*` (namespace `files`); name rules, path
building and the erased-flash check are the pure component `components/files`
(host-tested). Library: `joltwallet/littlefs` (version pinned in
`components/core/idf_component.yml`).

- **Partition:** the first data partition of subtype `littlefs` (both
  partition tables have exactly one), mounted at `cfg::kFsBasePath` (`/fs`).
  The partition is found by type, so its label is not repeated in the code.
- **Mount never formats.** `format_if_mount_failed` is off. When the mount
  fails, the service reads the whole partition:
  - every byte erased (`0xFF`): storage that was never used, for example
    after a full flash erase. It is initialised (formatted and mounted) with
    a warning in the log (`initialised and mounted`);
  - anything else: the content is kept unchanged, the state is
    `kUnavailable`, an error is logged. Only the explicit reset
    (`files::format()`) erases it. A damaged filesystem therefore never
    destroys images that might still be recovered, and a mount fault never
    counts as permission to format.
  The check stops at the first programmed byte; a fully erased 448 KB
  partition takes 133 ms on the XIAO ESP32-C6 (only on this path).
- **API:** `init()` (once at boot, after the settings store; never fails),
  `state()` (`kMounted`, `kUnavailable`, `kNotStarted`), `usage()`,
  `format()` and `logStatus()` (part of the periodic status log). Files are
  accessed with the standard POSIX/stdio functions on paths from
  `files::buildPath()`.
- **Names:** flat, 1–31 characters of `a–z 0–9 _ - .`, not starting with a
  dot. No directories, no `..`, no upper case. Names starting with a dot are
  reserved for the service's own replacement temporary file. Longest path: `/fs/` + 31 characters (36 B with terminator).
- **Image layer:** quota, replacement reserve and reader-aware publication
  are implemented in `components/images`; see [WEB_UI.md](WEB_UI.md).
  That same service owns one desired/selected image, file validation,
  source-attempt acknowledgement, role/factory fallback and bounded retry.
  Selection uses the shared heavy admission outside LVGL/data locks; status
  is a coherent bounded snapshot. UI keeps desired references/slide positions
  and renders the content error independently of health/OTA. Display wraps
  the existing JPEG/FS callbacks with exact source/version/config guards,
  observing actual block delivery without another decoder or retry policy.
  Missing/invalid file content never causes automatic deletion/settings save.
  Formatting acquires the LVGL lock and invalidates image cache readers.
  Trial firmware blocks persistent mutations. The lower-level format API
  still requires that no caller owns an open file.
- **Memory (XIAO ESP32-C6, 2026-10-09):** flash +42,048 B in total
  (`firmware.bin`; the LittleFS library 31,818 B, the rest the service and
  the C-library/VFS file functions it pulls in); static RAM +240 B (DIRAM
  +466 B). Heap: mount 1,660 B (peak 2,004 B); an open file 936 B more
  (LittleFS file cache 512 B plus records); after the first file access
  264 B stay allocated (not attributed; constant over repeated boots and a
  format). A failed mount leaves 88 B allocated.
- **Device tests** (`cfg::kFileTest` in `app_config.h`, back to `kNone`
  afterwards; they destroy the filesystem content, never run them on owner
  data). Each spans two boots; press RST in between:
  - `kCorrupt`: overwrites both superblock copies (blocks 0 and 1). Next
    boot: `PASS: damaged filesystem kept, initialised at boot: no`.
  - `kErase`: erases the partition. Next boot: `PASS: erased partition
    initialised, 8192 of 458752 B used`.
  - `kFormat`: writes and verifies a 4 KB file. Next boot: the file is read
    back, the filesystem formatted, the file must be gone:
    `PASS: file after restart intact; format ESP_OK; file gone; ...`.
  A populated filesystem (an image written by an earlier build) mounted
  unchanged. All passed on the XIAO ESP32-C6 (2026-10-09).

## Time service (implemented, offline device-tested)

`components/core/time_service.*` owns wall-clock validity and the global
time zone; `components/timekeeping` provides the pure label lookup and
local daily-window predicate. It uses the firmware's Picolibc via standard
C-library APIs and `esp_timer_get_time()` for monotonic milliseconds.

- Initialised after the event bus and settings store; no dedicated task.
- A static mutex serialises `TZ` changes, conversion and validity state.
  Only the service may set `TZ`; callers use its conversion APIs.
- Validity begins false on every boot. A successful `setTime()` records the
  source, monotonic set time and set count, then posts `kTimeChanged`.
  Zone changes post the same event; subscribers fetch the current state.
  The step count rises only for the first valid set, a zone change and a
  jump above `kTimeJumpThresholdS` (2 s); the provider service starts a new
  baseline only when it changed, not at every SNTP resync.
- A settings event callback only raises an atomic flag. The app task copies
  the settings (844 B in this candidate), applies a changed zone and updates diagnostics;
  this work runs outside the `events` task.
- Civil windows follow local time, including midnight and skipped/repeated
  daylight-saving hours. Delays and deadlines use monotonic time.
- The C6 offline test passed 112 zone-rule cases, 12 real-clock window cases,
  backward/forward jumps and notification/validity checks. Details and test
  selector: [NETWORK.md](NETWORK.md#time). SNTP wiring and night-mode rendering
  are implemented in this candidate; their network/UI acceptance is pending.

Before the health/FreeRTOS trace extension, the time-foundation C6 build
was 512,800 B (`firmware.bin`), an increase of 12,256 B over the previous
500,544 B file-service build. Its target size report was 512,170 B flash
and 47,136 B RAM (+352 B RAM); DIRAM was 92,010 B (+348 B). This included
the settings extension, fifth event
kind and time service; no new task is created. The time diagnostic and its
fixtures are removed from normal builds by the `kNone` selector. Cycling all
zones in that diagnostic retains 648 B of heap; that measurement is distinct
from normal single-zone initialisation (52 B measured). In that historical
build, heap after time init was 418,456 B; after display init and at 30 s
it was 389,380 B with a 368,640 B largest block. Main/events stack minimum
free was 2,464/1,940 B in that run.

## Health service (implemented; C6 console acceptance passed)

`components/core/health_service.*` runs in the existing app task; the pure
`health_meter.*` coordinator also has native tests. Design: ADR-021.

- **Console:** the existing stored `debug_status_log` and
  `debug_status_interval_s` fields control periodic reports (on, 30 s by
  default; 5–3600 s). A settings callback raises an atomic flag; the app task
  copies settings and applies them. Monotonic deadlines schedule reports.
  Disabling these reports preserves boot/state/error logs and supervision.
- **Report:** internal 8-bit heap free/largest/minimum values, all-task stack
  low-water marks, LVGL memory when its short lock can be acquired, and event,
  file and time status. The closing report line names the last reset reason
  (`esp_reset_reason`, e.g. power-on, software restart, panic, task watchdog,
  brownout); GET `/api/v1/status` returns the same text as `reset_reason`.
  Snapshot pause and complete report durations are
  recorded, including the final console write. Console output can delay the
  app loop; its measured duration is part of the acceptance budget.
- **Task snapshot:** two fixed tables hold at most 16 tasks (1,024 B together
  on the C6: 16 SDK records of 36 B and 16 owned rows of 28 B). On the C6 the
  scheduler stays suspended while collecting task data, reading the public SDK
  snapshot bounds and copying names into owned rows; formatting happens after
  it resumes. The usable span is the inclusive kernel range after port
  alignment, not the original allocation request (12,800 B requested gives
  12,785 B usable). The same mechanism covers application and SDK tasks without
  an allocation registry or task-name mapping. An incomplete or over-capacity
  scan produces no rows; the console reports the limit, and the status API
  returns an empty array. On multicore targets only copied numeric IDs and
  watermarks are exposed; names stay empty and usable capacity is 0, explicitly
  unknown, because suspending one core does not prevent another core deleting
  a task. S3 acceptance is pending.
  FreeRTOS trace is enabled for this API; CPU runtime statistics and the
  FreeRTOS text-formatting helpers remain disabled.
- **App-loop watchdog:** after startup work and boot diagnostics finish,
  `app_loop` is registered with the existing task watchdog (15 s, see
  display failures below). Each completed
  polling iteration feeds it after status work, including with the console
  disabled. A separate timer cannot hide a blocked app loop. Existing IDLE
  and LVGL supervision and the RTC abnormal-reset/headless policy are
  unchanged. This heartbeat does not prove event-subscriber progress.
- **Heap ownership:** only the shared coordinator calls the SDK's global
  local-minimum start/stop functions. A noncopyable RAII meter makes one
  atomic acquisition attempt without waiting or retrying. Nested/contending
  meters capture point values and elapsed time only; they never start or
  stop the owner's interval. `finish()` is idempotent. A start failure frees
  ownership; the owner reads the final minimum before stopping and releases
  ownership only after a successful stop. Stop failure quarantines the
  monitor until restart, leaving subsequent meters point-only.
- **Minimum scope:** observations explicitly distinguish lifetime, interval
  (with monotonic interval-start time) and unavailable. Lifecycle/generation
  checks around the sample and atomic timestamp publication prevent a
  transition from being labelled as a stable interval. Unavailable minima
  are zero with that explicit scope; free/largest/time remain available.
  The SDK sums per-region low-water marks: this is a conservative bound,
  not a simultaneous total or a measurement attributable to one caller.
  An owned interval includes allocations by every concurrent task.
- **Web observation:** `/api/v1/status` calls that same coordinator once for
  free/largest/minimum heap values and publishes `heap_min_scope` plus
  `heap_min_start_ms`. An unavailable minimum is JSON null, rather than the
  internal observation's sentinel zero. The Web task uses `stackMinima()` for
  a complete all-task reading: its temporary SDK-record buffer is freed before
  return, and the caller owns the 16-row result on its stack. It exposes both
  `stack_min_free` and `stack_usable_bytes`; zero usable bytes remain unknown.
  The lifetime reserve is 40,960 B, the largest-block reserve is 32,768 B, and
  each task needs at least 25% of its usable span free. Current largest-block
  samples cannot establish a continuous lifetime minimum.
- **Running-image ownership:** the OTA service parses the running partition's
  SDK image metadata once during its existing boot initialization and caches
  only a validated nonzero length (4 B on C6). `/status` exposes it as
  `app_bytes`, or JSON null if unavailable. The length includes image headers,
  segment data, checksum alignment and appended hash; the SDK metadata parser
  does not include signature-block bytes. Current targets use unsigned images.
  Parsing skips segment contents/checksum calculation and does not rehash or
  independently verify the image. Existing `/schema` `ota_max` remains the
  actual inactive-slot capacity; both supported layouts use equal app slots.
  The acceptance limit is at most 85% of that capacity. No second slot-size
  constant or external firmware-size metadata is introduced.
- **SDK correction:** `scripts/heap_monitor_patch.py` and `.cmake` replace
  only the heap source in the build with a local corrected copy. Exact
  ESP-IDF 6.1.0 and original-source hash checks fail closed on upgrades; the
  shared installed SDK is untouched. Failed monitor bookkeeping allocation
  returns `ESP_ERR_NO_MEM` before resetting heap minima, rather than asserting.
  Abort-on-allocation-failure must stay disabled. This corrects that one SDK
  path and does not promise recovery from every firmware allocation failure.

There is no new normal-runtime task or task stack, settings field, NVS format
or partition change. The fixed task tables need no heap allocation; the C6
coordinator object is 28 B; a phase-meter object occupies 96 B on its
caller's stack, in addition to temporary call frames.
Starting an SDK interval allocates one 4 B minimum value per registered heap
plus allocator overhead; the watchdog user also needs an SDK allocation.
Enabling trace affects every applicable FreeRTOS control object, including
objects allocated by libraries:

| Control object (C6) | Additional bytes per object |
|---|---:|
| Task / static task | 8 |
| Queue or semaphore / static equivalent | 8 |
| Event group | 4 |
| Software timer | 4 |
| Stream or message buffer | 4 |

These are structure costs, not a complete product resource acceptance.
Historical C6 console/device acceptance passed on 2026-10-10 for the earlier boot and
core-service load: a complete 16-task snapshot, clean overflow at 17 tasks,
worker cleanup, targeted monitor allocation failure/retry, overlapping meters,
32 repeated intervals, console on/off/on and one app-loop watchdog restart.
The one-shot restart resumed automatically, skipped another injection and
cleared the abnormal-reset counter after 60 s of display-task progress.

The 16-task scheduler pause was 1426–1428 us. A diagnostic report with seven
tasks, a busy display lock and 50 ms simulated delay per console line took
709735 us (14 lines), below the watchdog timeout of that build (5 s at the
time, now 15 s). In normal firmware,
a serial client closed for 70 s and reopened without control-line changes
preserved uptime and report counts; the maximum complete report was 53998 us.
Disconnected output can be dropped or truncated by the SDK. On the tested
macOS/pyserial setup, a USB reset occurred when pyserial reopened the port
and wrote DTR/RTS; such an observer cannot prove uninterrupted operation.
DTR/RTS are host serial control signals also used for reset; the accepted
observer only opened, read and closed the port. The USB cable remained attached in that test.

That earlier normal image was 516304 B, static RAM 48220 B and DIRAM 93090 B,
unchanged by its console acceptance session. Steady free internal 8-bit heap was 388012 B,
largest block 368640 B and the reported lifetime minimum 388012 B; the
minimum still has the SDK region-sum meaning above. That earlier normal
image was 3504 B larger and static RAM 1084 B higher than the earlier time-only
build. Future workloads need their own peak heap, fragmentation, stack and
latency acceptance. `cfg::kHealthTest` is `kNone` in normal builds; diagnostic
workers, hooks and the one-shot marker are absent from the normal ELF.
The candidate adds a separate controlled WebSocket status producer with no
console hook or retained history; see [WEB_UI.md](WEB_UI.md#live-debug).
Its inactive/active, slow-client and concurrent-load budgets remain unaccepted.

## Display fault handling (implemented, device fault tests pending)
- `esp_lvgl_port` stays unchanged (ADR-015). All handling lives in
  `components/display` (own GC9A01 driver and `display_port.cpp`); limits in
  `include/app_config.h`.
- Failed draw: the driver sends a `NOP`. The SPI panel IO collects every
  queued transfer before a parameter command, so afterwards the LVGL buffer
  is free. The port then reports the flush complete to LVGL and marks the
  screen for a redraw (invalidating is not allowed while LVGL renders). The
  supervision timer, nominally every 500 ms in the LVGL task, invalidates the
  screen on its next run; it can run later while the LVGL task is rendering,
  waiting for transfers or held off by the LVGL lock. The failed area is
  correct again only after that redraw has been rendered and sent, which
  takes as long as a full redraw (with a JPEG background 1.1–1.25 s, see
  below). There is no fixed recovery deadline. After 3 consecutive failed
  draws the device restarts. If the drain fails too, the buffer state is
  unknown, so the device restarts immediately.
- LVGL supervision: an LVGL timer (500 ms) feeds a task-watchdog user. A
  stuck flush, a blocked transfer, a long-held LVGL lock or an LVGL assert
  stops it, and the task watchdog (15 s, panic enabled in `sdkconfig.defaults`)
  restarts the device. Never hold the LVGL lock for long work. The timeout is
  one value for every watched task (IDLE, `lvgl`, `app_loop`), so a stalled
  display or app loop is detected up to 15 s after the stall, not 5 s.
  15 s is an interim value: at 5 s the IDLE task was starved while the
  provider task ran a TLS handshake (reset at first boot). The clean solution
  is open and tracked in the task plan.
- Allocation: before `lvgl_port_add_disp`, the port's heap allocations are
  probed (allocated and freed again): its context with an upper-bound size
  (256 B) and the two DMA draw buffers with their size, alignment and caps.
  A shortage restarts the device in a controlled way. The probe is best
  effort: it reserves nothing, so another task can allocate in between. If
  the port's context allocation still fails, `esp_lvgl_port` 2.9.0 does not
  handle that safely (its error path dereferences the missing context) and
  the device ends in a panic restart. An LVGL pool shortage
  ends in the LVGL malloc assert (`CONFIG_LV_USE_ASSERT_MALLOC`) and then in
  the watchdog restart.
- Init failures (SPI bus, panel IO, panel, clear, LVGL, supervision) restart
  the device after switching the backlight off. The reset releases every
  resource, so there is no reverse-order cleanup. A clear buffer whose
  transfer state is unknown is not freed.
- Controlled restarts use `esp_system_abort` with the reason in the panic
  output. A reset record in RTC no-init memory counts consecutive abnormal
  resets (panic and watchdogs). After 3 of them the display is not
  initialised and the device runs headless until a power cycle or a normal
  restart. After 60 s of stable display operation the count resets.
- Profile resolution, SPI mode and clock are checked at compile time
  (`include/hw_target.h`).
- Fault injection: `cfg::kDisplayFault` compiles one test fault in (it must be
  `kNone` in normal builds). `cfg::kDisplayTest = DisplayTest::kColourTest`
  shows the developer colour test (red outer ring, green inner ring, white
  texts, version in blue) at boot instead of the start screen; normal builds
  do not link it. A multi-chunk partial transfer is not injectable
  without changing ESP-IDF. It cannot occur with the current sizing (one
  flush = one SPI transaction), and it uses the same drain path.

## Memory strategy (C6 has no PSRAM)
- Fixed-capacity model containers, allocated once.
- Bounded, measured library allocations and LVGL-pool allocations are allowed.
  Avoidable general-heap allocation churn remains prohibited. Event payloads,
  filtered parser documents and screen transitions still need peak and
  allocation-failure budgets.
- One persistent scene is reused for every screen: one image object and one
  full-screen draw object that paints a fixed list of at most 72 items (text,
  pill, arc, dots; texts in a 1,280 B arena). The application invalidates
  added, removed and changed item bounds; unchanged items add no invalidation.
  LVGL can merge dirty areas, and initial frames, screen transitions and
  image/style/rotation changes can repaint the whole screen. Partial DMA
  buffers still transfer it stripe by stripe; JPEG decoding can restart
  from the top for each stripe. The fixed list/arena, temporary LVGL draw
  tasks, DMA buffers and decoder work state are separate resource costs.
- LVGL draw buffers partial, double buffered, internal DMA-capable RAM:
  1/12 of the screen height each (240×240: 2 × 20 rows = 2 × 9600 B), size
  computed from the profile (`components/display/display_port.cpp`).
  LVGL heap `LV_MEM_SIZE` = 32 KB static (`.bss`), see `sdkconfig.defaults`.
  The candidate keeps these limits. Full-screen/background/font/object peaks
  remain unmeasured; the old boot measurement does not prove sufficiency.
- JSON streams through a bounded array walker. Relevant records are mapped
  one at a time; skipped fields still count toward byte, depth and allocation
  bounds. A full response is never buffered. Every value goes through
  ArduinoJson; a number has no end marker, so while one is parsed the stream
  ends before the first character that cannot belong to it, and the separator
  after it stays for the walker.
- TLS: one connection at a time.

### Staged resource acceptance
The active C6 target is accepted in stages: baseline integration (WiFi/TLS,
filtered JSON, JPEG and HTTP server), complete data model and production views,
images/Web/debug operations, then supported maximum load and long-run operation.
Passing the first stage does not certify the complete application.

Heavy admission serializes TLS/parsing, image validation/mutation and OTA.
Routine LVGL JPEG redraw can still overlap provider traffic inside its fixed
pool. That overlap is implemented but has not been accepted for the full load. Before each stage, define capacities, overflow behaviour and
explicit reserves. Account for linked code/data SRAM,
LVGL pool (without counting it twice), DMA buffers, all task stacks, the match
store and reader copies, strings/events, queues, parser and TLS peaks,
view models, JPEG/layers, embedded Web assets, the NVS settings record, and filesystem
metadata/upload staging. Measure total free memory, largest free blocks, low-water marks,
stack usage and LVGL peaks/fragmentation with the permitted overlapping work.
Compare image occupancy and resource growth with the previous measured stage.
The complete C6 budget remains unproven until these workloads exist.

JPEG integration must include its filesystem adapter and the RGB888 input
conversion required by TJPGD when drawing to RGB565. Decoder output format does
not require a permanent full-screen RGB888 framebuffer. Measure actual decode
time, working memory, LVGL peak and flash growth using baseline JPEG assets.

First measurement (XIAO ESP32-C6, 2026-10-09, temporary diagnostic build that
was removed afterwards and remains in the Git history): the
baseline stage fits the current partitions with reserves (WiFi, TLS with the
full IDF certificate bundle, filtered JSON, LittleFS, JPEG scene and a small
HTTP server: 78 % of an OTA slot, largest free heap block ≥ 180 KB, minimum-ever
free heap ≥ 170 KB with TLS running while the scene redraws). Three findings
shape the image design:
- The C6 ROM linker script defines `jd_prepare` and `jd_decomp` (an older
  TJpgDec with a different layout) and they override LVGL's TJPGD. LVGL's
  functions must be renamed at build level (compile definitions for the LVGL
  component), otherwise the first JPEG draw crashes.
- LVGL's TJPGD decoder re-decodes the image from the top for every draw task
  (one per draw-buffer stripe): about 105 ms per 240×240 pass on the C6, so a
  full-screen redraw over 12 stripes takes 1.1–1.25 s. Text-only refreshes of a
  JPEG background cost 0.2–0.3 s. Fewer, larger stripes, a pre-rendered layer or
  uncompressed RGB565 images trade this against RAM or flash. Because LVGL 9.6
  style setters and `lv_display_set_rotation` invalidate even when the value is
  unchanged, the scene and the display port apply styles, alignment and
  rotation only when they change; otherwise every refresh is a full redraw.
- At that historical measurement, LVGL 9.6's TJPGD lacked an end-of-image
  check: walks ended with a benign format warning. The current guarded
  callback wrapper observes successful final-block delivery and returns
  normal end before requesting an extra block; the old warning suppression
  is removed, so genuine warnings remain visible. The installed decoder and
  codec are delegated unchanged. Full-file validation remains separate.
- LittleFS cannot replace a file that is open, and a draw task keeps the JPEG
  open while it decodes. An image upload must swap the file under the LVGL lock
  (temporary file, then rename), otherwise a replacement during a redraw fails.
This is integration evidence only, not the supported-maximum envelope.

### Current compiled resource ledger — 2026-10-16 (one source per competition, no fixture pairs, no settings migration, no core-data request, one job outcome protocol; compact match store, request scheduler and plans in the firmware, competition files and short names, Matchday screen and per-competition screen switches, one matchday module with current/next selection, status per competition with error codes and status pill, store envelope of 224 matches and rows with a 64-row table stage, 48 events and the store-full notice E19; settings format 9 with competition store and request plans, image roles, font set 12/14/18/48, display design renderer, start screen, automatic season)

| Item | C6 candidate | Growth over the accepted core image |
|---|---|---|
| Firmware binary | 1,650,176 B | +1,133,872 B |
| PlatformIO flash report | 1,649,420 B, 89.9% of 1,835,008 B | +1,133,756 B |
| Static data/BSS | 150,940 B | +102,720 B |
| Linked DIRAM, official IDF size report | not re-measured (266,066 B at the 2026-10-10 image) | — |
| Match store | 26,712 B (BSS) | Included above; replaced three snapshots of 27,200 B (81,600 B) |
| Short-name table | 48 entries of 32 B (BSS) | Included above |
| Football files on LittleFS | about 3 KB (one block) per competition, short names up to 1,552 B | Budget 48 KiB for all football files |
| Settings model / longest encoded record | 832 / 806 B | Format 9 (variable length, ceiling 2,048 B); formats 1 to 8 are not read (reported as outdated, initial values apply, the old record stays until the next save) |
| Competition store / longest encoded record | 1,226 / 1,234 B | Own NVS record, format 2 (ceiling 4,096 B); 8 entries of 153 B, one source each; format 1 is not read (outdated) |
| UI view model / draw list / text store | 1,880 / 2,016 / 1,280 B | Static owned view; 72 draw items of 28 B; view and draw list are initialised data (also stored in flash) |
| Parser live allocation cap | 24,576 B including custom headers | Libc metadata additional |
| JPEG validation workspace | 4,096 B static | Included above |
| Stock JPEG files / filesystem image | 7,862 / 458,752 B | Logical files / gross partition |

The DIRAM split (179,000 B BSS, 17,736 B data, 69,330 B RAM-resident code) is
from the 2026-10-10 image and was not re-measured. The certificate bundle is
the common-CA set of 40 roots (16,738 B); the full Mozilla set (67,817 B) was
replaced to save flash (verified at build level; TLS to every provider host
still needs a device log). Assertions are silent in release builds. No
partitions, public features or library versions were removed to fit the slot.
Runtime initialization of large snapshot defaults, shared stock aliases/font
roles and unused C6 SDK speed/protocol features reduced the first complete
build's 94.3% occupancy to 81.8% at the settings-format-3 build.

The image fits the OTA slot and is 53,119 B above the 85% reserve line
(1,559,757 B) using the PlatformIO report; the line may be exceeded during
the display redesign, and the savings are decided at alpha from a size map. Settings format 3 added 3,706 B flash
and 620 B static RAM over the preceding build; the later Web UI rewrite, API
and provider changes grew flash to 84.6%, and settings format 4 with the
completed Web UI added 6,928 B flash (6,754 B of it embedded page gzip) and no
static RAM. The display redesign so far: removing the input reset −412 B,
Konferenz blocks and pages +1,660 B, settings format 5 with input roles
+1,184 B flash and +48 B static RAM (three static model copies +12 B
each; the other 12 B are not itemised), their Web
UI fields +1,536 B (embedded page 50,019 B gzip), the mockup font set
−7,688 B, and the new renderer and screen binding +15,302 B flash and
+3,736 B static RAM (largest parts: screen binding about 5.2 KB, renderer
about 4.2 KB, LVGL arc drawing about 3.3 KB, the night gamma `powf` about
2.8 KB, view model and draw list as initialised data 3.9 KB). The UI
binding runs on the main task (8 KB stack) with a 368 B frame inside the
2.1 KB frame of the UI poll. The next-retry time in the snapshot added
266 B flash and 16 B static RAM. The start screen in place of the colour
test saved 7,386 B flash and no static RAM: the colour test was the last
user of the LVGL label widget and flex layout, and it is linked only when
`cfg::kDisplayTest` selects it. The SVG previews of the redesigned display
in the Web UI added 4,472 B (embedded page). Image roles (replaceable stock
crest and background) added 458 B flash and no static RAM; settings format 6
with the crest hand-over and the matching Web UI change added 798 B flash and
saved 16 B static RAM (embedded page 54,619 B gzip); the role API (list,
role upload, copy/reset endpoint) added 1,310 B flash and no static RAM; the
image page role tiles (replace, copy from the library, restore, delete-all
resets) added 1,200 B flash and no static RAM (embedded page 55,814 B gzip);
removing the upload buttons from the display page saved 320 B flash (embedded
page 55,496 B gzip). Settings format 7 with a daily cap and a minimum spacing
per provider (one request counter per provider, Web UI fields) added 2,140 B
flash and 96 B static RAM over the preceding build (embedded page 56,782 B
gzip, +686 B; the settings model grew by 16 B per copy). The next-round
time and reason in `/status` and the Web UI overview added 1,132 B flash and
8 B static RAM (embedded page 57,364 B gzip, +582 B). Settings format 8
(eight competitions with stable ids and screen switches in their
own NVS record, per-provider daily cap, window limit and six intervals, a
global limit, the competitions endpoint and the Web UI list) added
15,216 B flash and 2,928 B static RAM over that build (embedded page
60,035 B gzip, +2,671 B); the plan estimated +6…10 KB flash and about
−0.5 KB static RAM. The settings model shrank from 1,968 B to 824 B, but the
competition store (2,459 B) and its copies are new; the itemisation of the
growth is not measured. The compact match store with the request
scheduler, request plans and store views in place of the snapshots added
13,900 B flash and saved 56,492 B static RAM (estimate: flash −10…0 KB,
static RAM −55…65 KB). Largest code parts: provider worker 17.0 KB, store
mappers 11.3 KB, match store 8.5 KB, request plans 7.7 KB, scheduler
5.0 KB, store views 4.6 KB; the old snapshot code is no longer linked. A
static store with an implicit constant constructor would be initialised
data (+22.7 KB flash), so its constructor is out of line. The competition
files and short names (file codec, load at boot, write and sweep, the
`/teams` endpoints and the Web UI dialog) added 12,030 B flash and 2,128 B
static RAM (estimate: flash +4…7 KB). Largest parts: provider worker
+4,971 B, file codec 3,747 B, web service +2,308 B, C-library file
functions about 0.5 KB, scheduler and plans +826 B, embedded page +372 B
(60,984 B gzip). The images-first rule for football files and the short
names in export and import (`PUT /teams`, Web UI import of three parts) added
2,042 B flash and changed static RAM by −4 B (embedded page 61,321 B gzip,
+337 B). Deleting the unlinked snapshot model, its mappers and the old
Konferenz code changed flash by −42 B and static RAM by +4 B: that code was
no longer linked, so only header-level leftovers went.
The number fence of the JSON walker added 176 B binary; the connection stage
in the failure log, one repeat of a selection list and the early end at a full
page added 368 B binary; the search filter of the Web UI added 448 B (embedded
page 61,769 B gzip). Static RAM did not change.
Three request-log fixes (final table fetch after boot, one table request for
table and live table, resetting only competitions of a changed provider)
added 318 B flash. The Matchday screen, the per-competition screen switches,
the table cycle with its step time, the live-table use and the ESPN
substitutions from the match summary added 5,270 B flash and 152 B static
RAM (embedded page 62,843 B gzip, +1,074 B; estimate flash +6…10 KB, gzip
+1…2 KB, static RAM +0.6 KB); the settings model grew to 844 B. Three
corrected Web UI texts (window limit, daily-limit reason, daily-cap help)
added 184 B flash (embedded page 63,026 B gzip, +183 B).
One source per competition (no fallback source, no fixture pairs, no
combining in the match store, the scheduler without the source dimension)
and the removal of every settings migration (formats 1 to 7 and competition
format 1 are reported as outdated) changed flash by −19,444 B and static RAM
by −4,424 B (estimate flash −15…−27 KB, static RAM −3…−5 KB); the embedded
page went from 63,052 B to 59,963 B gzip (−3,089 B). The competition store
shrank from 2,459 B to 1,226 B per copy, the planner is 1,256 B.
Removing the core-data request (team names come with fixtures and table;
API-Football re-resolves its season with the first fixtures run after
24 hours; settings format 9 without the core interval) changed flash by
−574 B and static RAM by −88 B (estimate flash −1…−2 KB, RAM about
−100 B); the embedded page grew from 59,963 B to 60,114 B gzip (+151 B,
the backup-import note in two languages). The settings model shrank from
844 B to 828 B, the longest settings record from 821 B to 805 B.
One job outcome protocol (typed outcomes in the scheduler, the request
counted at its real HTTP start, no wait loop in the provider request, the
owner's selection list as a resumable work item with its wait in the API,
the failure backoff up to 15 minutes, a damaged request counter settled on
the first valid day, the exact cost of the API-Football season run)
changed flash by +1,254 B and static RAM by +96 B (estimate flash
+1.5…3 KB, static RAM +4…40 B; the selection work item with its season
buffer and the wait fields account for the RAM); the embedded page went
from 60,114 B to 60,437 B gzip (+323 B, the wizard wait line in two
languages). One search text per wizard step then added flash +24 B and
27 B gzip to the embedded page (static RAM 0).
One matchday module (gap, over, passed and the current/next selection in
one place for store expiry, views, planner and UI), the `matchday_mode`
setting, Konferenz membership in one pass and the Up/Down stepping through
pages, matchdays and competitions changed flash by +1,956 B and static RAM
by +16 B (estimate flash +0.3…1 KB, static RAM +0…8 B; the settings model
grew from 828 B to 832 B through alignment, held in three static copies,
plus the 4 B held matchday of the UI); the embedded page went from
60,410 B to 61,074 B gzip (+664 B, estimate +0.2 KB; the select with its
help in two languages). The longest settings record grew from 805 B to
806 B.
Status per competition (a state with cause, provider, HTTP status and last
update per competition instead of one global error, stable error codes,
notice cards in the Matchday and Table cycles, the status pill and the Web
UI cause and action texts) changed flash by +7,138 B to 1,623,248 B and
static RAM by −108 B to 146,604 B (estimate flash +1…2 KB, static RAM
+50…100 B; the static RAM reduction is not itemised); the embedded page went from 61,074 B to 64,321 B gzip
(+3,247 B; the cause and action texts in two languages).
Moving the mapping of HTTP statuses and transport results to causes into
host-compiled football code (`fetchCause`, `errorCause`, `errorStatus` in
`football_model`) and three Web UI fixes (the header connection chip in
the error state, provider display names in the overview, four unused
translations removed) changed flash by +10 B to 1,623,258 B and static RAM
by 0 B (estimate ±0); the embedded page went from 64,321 B to 64,236 B gzip
(−85 B).
The store envelope (224 matches and table rows, 240 teams, 48 events, a
64-row table stage, the store-full notice E19 with per-kind clearing)
changed static RAM by +4,048 B to 150,652 B (estimate +3.9 KB; the store
grew by 3,984 B) and flash by +1,976 B to 1,625,234 B (estimate +1…2 KB);
the embedded page went from 64,236 B to 64,475 B gzip (+239 B; the E19
texts). Replayed real ESPN fixtures and tables fill the store exactly with
4 × Champions League + 4 × Premier League (224 matches, 224 rows, no
notice); bodies stay at most 202,542 B (limit 256 KiB). A table row parses
to at most 10,310 B, a scoreboard match to up to 32,094 B on the 64-bit
host, above the 24 KiB parser cap; the C6 figure is not measured.
The following review corrections (provider ids and mapping, Web validation,
imports and sessions, terminated draw-list strings, JPEG timeout reporting,
network/time retries and event ordering) add 1,502 B flash and 48 B static
RAM over that store-envelope build. The embedded page is 64,964 B gzip
(+489 B); LittleFS use is unchanged. The 2026-10-16 verification build of
`f233dd4` reproduces these figures. The subsequent AP-state correction adds 32 B flash with no static RAM.
The later integration build, including resource-status metadata, serialized
image maintenance, the diagnostic display, the approved ESPN element
filter, complete table-number widths and responsive diagnostic values,
measures 1,632,980 B flash / 1,633,728 B binary / 150,796 B static
RAM / 65,380 B embedded page. Compared with the verified `f233dd4` build,
this is +6,244 B flash, +96 B static RAM and +416 B page gzip. LittleFS is
unchanged. The native filter may skip malformed ignored scalar fields;
retained fields, body/depth/EOF and allocation bounds stay checked.
This checkpoint precedes the image-source retry and controller split.
With the image-source retry and the controller/navigation/presenter split,
after removing the obsolete displayed-page cache, the latest C6 build is
1,634,964 B flash / 1,635,712 B binary / 150,804 B static RAM. The page
stays 65,380 B gzip and LittleFS use stays unchanged. This combined block
adds 1,984 B flash and 8 B linked static RAM; its pure navigation state is
80 B, presentation state 24 B and a short-lived presenter 32 B on C6.
No extra settings/view model or heap allocation is introduced by the split.
The later full-version command transport and physical gesture cancellation
add 640 B flash and 16 B static RAM: 1,635,604 B flash / 1,636,352 B binary /
150,820 B static RAM. Both queues preserve the full settings generation;
pre-save commands are discarded and fresh queued commands survive reload.
The additional gesture flag fits existing boolean padding: each InputPolicy
stays 40 B. The existing four-entry UI FIFO grows from 16 to 32 B; requested
SDK queue storage grows separately by 164 B, as detailed above. There is no
new per-post allocation or LittleFS/NVS quota change. The reload compiler
frame is 864 B (16 B above the transport-only version); SDK call-chain and
runtime stack peaks are not inferred from that single frame.
The subsequent phase-recovery/status integration adds 5,948 B flash and
72 B linked static RAM: at that checkpoint, binary
1,642,304 B and page 66,540 B gzip (+1,160 B). The flash cost is 5.81 KiB,
above the preliminary 2…5 KB estimate; functional error explanations and
phase ownership are included. LittleFS/NVS quota and formats are unchanged.
Network status grows 76→88 B; raw network static symbols grow 63 B before
linker alignment. There is no second task or duplicated SDK steady-state
resource. Failed SDK construction may still allocate transiently; actual
allocator peaks require device measurement. The eight added status members need sixteen C6 variant slots (128 B),
plus a possible long-integer slot and copied diagnostic strings. A crossed
pool boundary requests another 1,024-B pool. The conservative additional
allocator request is 1,186 B before libc metadata, exceeding the earlier
0.1…0.4 KB estimate. Controlled actual-handler host comparisons (612 pairs,
75 crossed pool boundaries) plus C6 ABI derivation give at most 5,702 B for
the complete document in those fixtures under the unchanged 24,576-B cap.
The owner accepted this corrected incremental cost together with the
complete network recovery and existing JSON processing on 2026-10-16.
This is not a runtime heap/fragmentation measurement; concurrent peaks and
libc metadata are excluded. Largest tested response is 4,867 B, streamed
through the existing writer in chunks of at most 512 B.

Compiler frame comparisons with the previous checkpoint: network poll
1,712→1,600 B, previous settings application 1,296 B versus new lifecycle
1,328 B, publish384→400 B; UI poll224→240 B, status presenter288→304 B,
Web status1,424→1,440 B. These are individual compiler frames, including
existing work, not additional buffers or measured runtime stack reserves.
The already existing display startup flag uses bit16 in its packed atomic;
the returned status fits one ABI word, with no additional static state.

Initialization recovery is implemented through returned-error paths,
including initial mDNS hostname/HTTP advertisement. Lower-level SDK panics
remain fatal. All service/device acceptance and full runtime reserves stay
open.
The subsequent image-content recovery/observer/status integration, including
the shared warning pill and confirmed file-I/O error handling, adds 6,932 B
flash and 48 B linked static RAM: 1,648,484 B flash/1,649,232 B binary/
150,940 B static RAM/67,904 B page gzip. The page increment1,364 B is already
included in flash. This 6.77 KiB flash increment exceeds the combined
preliminary 6.4 KiB upper estimate; the owner accepted this complete cost on
2026-10-16. Hardware reserves and the later same-id repair/reservation
increments remain separate. LittleFS/NVS quotas and formats are unchanged. No new task,
own permanent heap or extra active decoder is introduced. The existing
decoder session requests 16 B more from the fixed LVGL pool, before its
allocator overhead. C6 ABI-derived extra temporary status allocation is at
most 1,282 B before libc metadata; complete controlled fixtures reach 5,873 B
and 5,156 B serialized, using the existing 512-B writer/24,576-B cap.
Selected overlapping project stack paths increase by 144 B (library) or
160 B (role), including UI poll, and 96 B for status presentation. These
compiler/disassembly bounds exclude SDK/codec/libc frames and are not runtime
stack reserves. Actual LCD output, late faults, concurrent heap peaks,
fragmentation and OTA acceptance remain hardware work. The binary is 89,476 B
above the 85% slot-reserve line.

The source/version contract for the installed decoder also runs before
cached PlatformIO builds. Owned CMake inputs and VERSION invalidate only the
generated build graph; SDK defaults retain their separate guard. This host
workflow adds no firmware memory, flash or LittleFS cost.
The later same-id repair and referenced-id reservation add 936 B flash,
0 B linked static RAM and 545 B page gzip (included in flash). The latest
binary is 1,650,176 B and page 68,449 B. No task, own persistent heap,
settings format or LittleFS/NVS quota change. Automatic allocation combines
a coherent stored-reference mask with the current browser draft mask;
explicit replacement restores its desired id. The new store getter does not
copy the settings model. The selected direct project upload path remains
1,552 B in both linked ELFs: begin's own frame grows 32→48 B but is released
before tail-calling start; the new getter branch uses 64 B. The enclosing
HTTP upload frame stays 1,152 B. These exclude SDK/libc/runtime peaks.
The reserve miss is now 90,420 B; cost approval does not accept this gate.
No flash-reserve gate is
accepted. Free heap is not derivable from the static figures: dynamic stacks,
network, TLS, parser, DMA and library charges must still be accounted for. Peak heap,
largest block, 25% stack reserves, LVGL fragmentation, ten-socket concurrency,
filesystem metadata and replacement space remain hardware acceptance work.


The parser cap is per allocator, not a global total: the single HTTP handler
can have its own 24 KB JSON document alongside the provider's 24 KB document.
During request parsing its bounded 8 KB receive buffer is additional. Basic
configuration/status requests are permitted during TLS; measure that overlap
along with normal JPEG redraw before accepting heap reserves.

## Scalability rules
- No absolute pixel values in views: positions/sizes from layout tokens
  derived from resolution, shape and size class.
- Round content must pass geometric safe-area tests; rendered visibility
  needs pixel-mask tests. Backgrounds, clipping containers, decoration and
  diagnostics have distinct contracts, detailed in [UI.md](UI.md). The
  diagnostic boot rings remain part of the accepted bring-up screen.
- Input-agnostic navigation: screens react only to `UiAction`s.
- New provider = new module implementing the provider interface + fixtures +
  capability flags; no change in presenters or views.
- New display/board = profile files + one env; a new controller also needs a
  driver. A new resolution requires its own DMA/draw-buffer, font, scene and
  flash/filesystem budget; hardware independence does not imply equal costs.

## Source layout

```
platformio.ini, CMakeLists.txt, VERSION, sdkconfig.defaults
dependencies.lock.<chip>     exact component resolution per chip
partitions/                  unchanged flash layouts
boards/ displays/ targets/   hardware/profile facts and SDK target settings
include/app_config.h         all software defaults and bounds
include/secrets.h            local ignored secrets/presets
src/                         boot wiring and the app/service loop
components/
  core/                      NVS store, event loop, files, time, health
  display/                   panel/LVGL port, JPEG validator, rotation/PWM
  events/ files/ geometry/    pure admission, paths/erased check, SafeArea
  settings/ timekeeping/     pure typed record/codec and civil rules
  network/                   WiFi policy, antenna, DNS, mDNS, SNTP, admission
  football/                  compact match store + row mappers, request
                             scheduler, provider request plans, matchday
                             rules (gap, over, passed, selection), views from
                             the store, competition and short-name files,
                             provider worker, client, budget
  ui/                        controller, pure input/navigation policy,
                             football/status presenters, views
  ui/fonts/                  generated fixed glyph subsets
  ui/lvgl_font/              LVGL default-font header (14 px text role)
  images/                    quotas, validation, cache lifetime and publication
  web/                       strict settings/API, upload and live debug
  system/                    firmware identity, trial health and rollback
web/                         offline browser sources
assets/src/ assets/fonts/     stock artwork and licensed font sources
data/                        generated stock LittleFS content
test/                        existing native suites; new acceptance is pending
scripts/                     configuration guards, native selection, heap patch,
                             formatting, asset/font/web builders, release guard
```
