# Architecture

> Status: **partly implemented**: hardware profiles, display port with fault
> handling (`components/display`), boot test screen (`components/ui`), pure
> SafeArea geometry (`components/geometry`) with native tests, the settings
> model and record format (`components/settings`, native tests), the NVS
> settings store (`components/core`) and `include/app_config.h` (defaults and
> limits). Everything else is planned.
> Decisions in [DECISIONS.md](DECISIONS.md). Planned contracts below do not
> describe implemented services or a completed resource acceptance.

## Goals
- Show football data for one club on small displays (first: 240×240 round).
- Scale to other boards, displays (360/466 px, rectangular), touch and
  additional data providers **without rewriting** screens or logic.
- Robust 24/7 operation: no hangs, bounded memory, safe updates.

## Layers

```
 ┌────────────────────────────── Web UI (browser) ─────────────────────────────┐
 │ tabs: Dashboard · Screens · Images · Settings · Update   (embedded, gzip)   │
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
 │ Repository (snapshot, change detection) · Scheduler (polling, budget,      │
 │ matchday window)                                                           │
 └───────┬────────────────────────────────────────────────────────────────────┘
         │ snapshot
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
| Repository | Hold current snapshot, detect changes (e.g. `lastScoreChangeAt`) | Network I/O |
| Scheduler | Choose polling interval per state, enforce request budget | Parsing |
| Screen-mode resolver | Pure function: (time, data state, settings, inputs available) → screen set + default | LVGL, I/O |
| Presenter | Build a view model for one screen from snapshot + settings | Draw |
| View | Create LVGL objects from layout tokens, render view model, emit `UiAction` | Business logic, data access |
| Input mapper | Map raw events (button press/long, touch gesture) to `UiAction` by device config | Navigation decisions |
| Navigation controller | Apply `UiAction` to screen stack / scroll state | Input hardware |
| Overlay manager | Show/hide overlays by priority and timeout, place them inside the round area | Know overlay content logic |

## Threading and data ownership
### Rules (planned unless marked implemented)
- **UI task**: owns LVGL exclusively. Other tasks never call LVGL; they post
  events. Views update only in this task.
- **Network/data task(s)**: HTTP requests and parsing.
- **HTTP server task**: from `esp_http_server`; hands work to other tasks via
  events/queues for anything slow.
- Communication via the event bus (implemented, below) and bounded queues.
  Each queue needs a capacity, post/allocation-failure handling and a
  coalescing or rejection policy. Commands requiring acknowledgement are
  direct calls with a result, never events; when overloaded they must return
  an explicit error and must not disappear silently.
- The repository has a single writer. Immutable snapshots need an explicit
  reader lifetime before their storage can be reused: two buffers alone do
  not protect a slow UI or Web reader. Readers take short protected copies of
  the data they need (ADR-017); the display task is the reader of live data,
  Web handlers only read small status/debug data. Copy sizes and lock times
  are measured when the repository is implemented.
- Model references and strings must outlive every reader and must never
  borrow storage from a discarded parser document. Each request carries a
  settings generation so an old club/provider request cannot publish into
  the new selection after a configuration change.
- **Current state (interim):** LVGL runs in the `esp_lvgl_port` task. The
  boot test screen is created once from `app_main` while holding
  `display::lock()` (recursive mutex of `esp_lvgl_port`). Exclusive UI-task
  ownership and its event protocol remain to be designed; until then every
  LVGL call outside the LVGL task holds that mutex.
- **Settings (implemented):** the current settings live in static RAM behind
  a mutex; `settings::current()` returns a copy (≈ 240 B on the reader's
  stack) and `settings::generation()` increases with every save or reset.
  Each save or reset also posts the event `kSettingsChanged`, so a service
  re-reads only after a change. Saves and resets are serialised by a second
  mutex and run in the caller's task (NVS flash writes block that task for
  milliseconds).

### Task model (ESP32-C6, single core)
Values checked in the ESP-IDF 6.1.0 sources and the generated sdkconfig of
`xiao_esp32c6_gc9a01`. A higher number means higher priority (maximum 24).
Planned tasks get their values when they are introduced, after measurement.

| Task | Priority | Stack | Created by | Status |
|---|---|---|---|---|
| `esp_timer` | 22 | 3584 B | ESP-IDF (high-resolution timer callbacks) | running |
| `sys_evt` | 20 | 2304 B | ESP-IDF default event loop (queue 32) | created with WiFi |
| `events` | 5 | 2304 B | event bus (`app_config.h`) | running |
| `taskLVGL` | 4 | 7168 B | `esp_lvgl_port` defaults, internal RAM | running |
| `main` | 1 | 3584 B | ESP-IDF, runs `app_main` and the diagnostic loop | running |
| `Tmr Svc` | 1 | 2048 B | FreeRTOS software timers | running |
| `IDLE` | 0 | 1536 B | FreeRTOS | running |
| network/data, HTTP server | – | – | planned | – |

The `events` task runs above the LVGL task so that a state change reaches
its subscribers before the next render. Its callbacks must therefore be
short (see below), or they delay rendering.

### Event bus (implemented, device test pending)
`components/core/event_bus.*`; the pure catalog and admission logic is
`components/events` (host-tested). Design decision: ADR-019.

- **Own loop:** a separate `esp_event` loop with its own task (`events`),
  not the ESP-IDF default loop. The default loop stays reserved for WiFi and
  IP events, so a slow subscriber here cannot delay the network stack.
- **Notifications only:** an event says *that* something changed, never
  *what*. The payload is at most 4 B (`uint32_t`), stored inside the queue
  entry; no post allocates heap. The receiver fetches the current state with
  a short copy from its owner (for example `settings::current()`).
- **Catalog** (`event_admission.h`): state events `kSettingsChanged`,
  `kNetworkState`, `kDataUpdated`, `kOtaState`; the action event `kUiAction`
  with the action code as payload. Only `kSettingsChanged` is posted today.
- **State events coalesce:** while one is queued, further posts of the same
  kind are absorbed (`post` returns `ESP_OK`). The pending mark is cleared
  just before the subscribers run, so a change made during a callback is
  queued again and never lost.
- **UI actions are bounded:** at most `kEventUiActionSlots` (4) are queued.
  A further post is dropped, counted and answered with `ESP_ERR_TIMEOUT`;
  the first drop of a burst logs one warning. Dropping a keypress under
  overload is preferred to a growing backlog of stale input.
- **Queue length** = number of state kinds + UI slots (8). A state event
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
- **Memory:** static ≈ 120 B (`.bss`); heap at init: task stack 2304 B plus
  the queue (8 × 16 B) and the loop and handler records — the boot log line
  `after event bus init` shows the real figure.
- **Diagnostics:** the status log prints the minimum free stack of the
  `events` task and the counters for dropped UI actions and failed posts.
- **Device test** (`cfg::kEventTest = EventTest::kFlood` in `app_config.h`,
  back to `kNone` afterwards): posts 20 UI actions to a subscriber that
  blocks 50 ms each, then 5 settings events. Expected:
  `PASS: UI actions 5 delivered, 15 dropped; settings events 5 posted,
  1 delivered; failed posts 0`. The test accepts every result within the
  bounds (at most slots + 1 delivered, at least one drop, coalesced settings
  events, no failed post).

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
  stops it, and the task watchdog (5 s, panic enabled in `sdkconfig.defaults`)
  restarts the device. Never hold the LVGL lock for long work.
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
  `kNone` in normal builds). A multi-chunk partial transfer is not injectable
  without changing ESP-IDF. It cannot occur with the current sizing (one
  flush = one SPI transaction), and it uses the same drain path.

## Memory strategy (C6 has no PSRAM)
- Fixed-capacity model containers, allocated once.
- Bounded, measured library allocations and LVGL-pool allocations are allowed.
  Avoidable general-heap allocation churn remains prohibited. Event payloads,
  filtered parser documents and screen transitions still need peak and
  allocation-failure budgets.
- One screen alive at a time; LVGL objects of the previous screen are deleted.
- LVGL draw buffers partial, double buffered, internal DMA-capable RAM:
  1/12 of the screen height each (240×240: 2 × 20 rows = 2 × 9600 B), size
  computed from the profile (`components/display/display_port.cpp`).
  LVGL heap `LV_MEM_SIZE` = 32 KB static (`.bss`), see `sdkconfig.defaults`.
  This is the current boot-screen configuration, not a proven production
  minimum or sufficient end-state budget.
- JSON parsed directly from the HTTP stream with a field filter (ADR-009);
  the full response is never buffered.
- TLS: one connection at a time.

### Staged resource acceptance
The active C6 target is accepted in stages: baseline integration (WiFi/TLS,
filtered JSON, JPEG and HTTP server), complete data model and production views,
images/Web/debug operations, then supported maximum load and long-run operation.
Passing the first stage does not certify the complete application.

Heavy operations (TLS fetch with parsing, JPEG decoding, upload handling) run
one at a time by default; overlaps are allowed only where measured and bounded
(ADR-017). Before each stage, define capacities, overflow behaviour and
explicit reserves. Account for linked code/data SRAM,
LVGL pool (without counting it twice), DMA buffers, all task stacks, snapshot
storage and reader copies/leases, strings/events, queues, parser and TLS peaks,
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
  uncompressed RGB565 images trade this against RAM or flash.
- LittleFS cannot replace a file that is open, and a draw task keeps the JPEG
  open while it decodes. An image upload must swap the file under the LVGL lock
  (temporary file, then rename), otherwise a replacement during a redraw fails.
This is integration evidence only, not the supported-maximum envelope.

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
platformio.ini
sdkconfig.defaults     ESP-IDF settings shared by all targets
dependencies.lock.<chip>  resolved component versions per chip target (tracked)
.clang-format          code style; requirements-tools.txt pins host tools
partitions/            partition tables per flash size
boards/ displays/ targets/   hardware profiles; targets/<target>.sdkconfig.defaults
                       = board-dependent ESP-IDF settings (ADR-010)
include/hw_profile.h   hardware profile types + compile-time pin checks
include/hw_target.h    selects the target (build flag), runs the checks
include/app_config.h   single software configuration (defaults, limits,
                       settings record version); grows with each feature
include/secrets.h      local secrets + personal presets (git-ignored)
src/                   entry point, app wiring
components/            ESP-IDF components = modules (core, net, web, data,
                       providers/*, ui, input). Existing:
  display/             SPI bus, panel IO, GC9A01 driver, esp_lvgl_port setup;
                       idf_component.yml pins lvgl + esp_lvgl_port
  core/                settings store on NVS (load, save, reset, presets,
                       device tests), event bus (own esp_event loop and
                       task); later file service, time, health
  events/              pure C++ event catalog and queue admission
                       (coalescing, UI slots); no ESP-IDF includes, host-tested
  geometry/            pure C++ SafeArea content bounds; no ESP-IDF/LVGL includes
  settings/            pure C++ settings model, limits, record codec (CRC32);
                       no ESP-IDF includes, host-tested
  ui/                  views (boot test screen); render a model, no logic
web/                   Web UI sources (embedded at build time)
assets/src/            high-res default images (sources)
data/                  generated LittleFS image content
test/                  native host tests + fixtures
scripts/               native build source selection, sdkconfig regeneration
                       and verification, format check (implemented)
tools/                 build scripts (web embed, asset conversion, boundary test)
```
