# Architecture

> Status: **partly implemented**: hardware profiles, display port
> (`components/display`), boot test screen (`components/ui`),
> pure SafeArea geometry (`components/geometry`) with native tests, and
> `include/app_config.h` (boot-layout/debug defaults). Everything else is planned.
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

## Threading and data ownership (planned)
- **UI task**: owns LVGL exclusively. Other tasks never call LVGL; they post
  events. Views update only in this task.
- **Network/data task(s)**: HTTP requests and parsing.
- **HTTP server task**: from `esp_http_server`; hands work to other tasks via
  events/queues for anything slow.
- Communication via event bus (`esp_event`) and bounded queues. Each queue
  needs a capacity, post/allocation-failure handling and a coalescing or
  rejection policy. Commands requiring acknowledgement must receive an
  explicit result when overloaded; they must not disappear silently.
- The repository has a single writer. Immutable snapshots need an explicit
  reader lifetime before their storage can be reused: two buffers alone do
  not protect a slow UI or Web reader. The choice between short protected
  copies of view models and reader leases on snapshot slots is still open;
  its RAM cost and writer-progress limits must be measured before use.
- Model references and strings must outlive every reader and must never
  borrow storage from a discarded parser document. Each request carries a
  settings generation so an old club/provider request cannot publish into
  the new selection after a configuration change.
- **Current state (interim):** LVGL runs in the `esp_lvgl_port` task
  (priority 4, stack 7168 B, port defaults). The boot test screen is created
  once from `app_main` while holding `display::lock()` (recursive mutex of
  `esp_lvgl_port`). Exclusive UI-task ownership and its event protocol remain
  to be designed; current external LVGL calls hold the mutex.

## Display fault handling (planned before integration acceptance)
- Keep the current display stack. A limited, reproducibly maintained patch
  of the pinned `esp_lvgl_port` will cover its flush-error and low-memory
  paths; generated managed-component files are not the source of the patch.
  Patch storage and the exact recovery mechanism must be decided before
  implementing it.
- Every submitted colour transfer needs a bounded completion/error contract.
  SPI panel IO can split a large colour transfer into smaller transactions;
  this does not make the caller's buffer safe to free on an error. In
  particular, partial submission may leave DMA reading it. Completion must
  be confirmed before releasing/reusing buffers, cleaning up or reinitializing.
- Initialization needs resource ownership and reverse-order cleanup for
  every failure stage, including the port, display context, both draw buffers
  and boot objects. Failed command/submission and missing/late callbacks must
  lead to a defined recoverable state without keeping LVGL's mutex forever.
- Validate profile geometry, SPI mode/clock and bitmap ranges before using
  them; keep controller software state consistent with successful writes.
  Fault-injection acceptance covers each allocation stage, partial transfers
  and safe retry. These are required repairs, not claimed device results.

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

Before each stage, define capacities, overflow behaviour, allowed concurrency
or serialization, and explicit reserves. Account for linked code/data SRAM,
LVGL pool (without counting it twice), DMA buffers, all task stacks, snapshot
storage and reader copies/leases, strings/events, queues, parser and TLS peaks,
view models, JPEG/layers, embedded Web assets, NVS generations, and filesystem
metadata/upload staging. Measure total free memory, largest free blocks, low-water marks,
stack usage and LVGL peaks/fragmentation with the permitted overlapping work.
Compare image occupancy and resource growth with the previous measured stage.
The complete C6 budget remains unproven until these workloads exist.

JPEG integration must include its filesystem adapter and the RGB888 input
conversion required by TJPGD when drawing to RGB565. Decoder output format does
not require a permanent full-screen RGB888 framebuffer. Measure actual decode
time, working memory, LVGL peak and flash growth using baseline JPEG assets.

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
partitions/            partition tables per flash size
boards/ displays/ targets/   hardware profiles; targets/<target>.sdkconfig.defaults
                       = board-dependent ESP-IDF settings (ADR-010)
include/hw_profile.h   hardware profile types + compile-time pin checks
include/hw_target.h    selects the target (build flag), runs the checks
include/app_config.h   single software configuration (defaults, limits);
                       currently boot-layout/debug defaults; full schema planned
include/secrets.h      local secrets + personal presets (git-ignored)
src/                   entry point, app wiring
components/            ESP-IDF components = modules (core, net, web, data,
                       providers/*, ui, input). Existing:
  display/             SPI bus, panel IO, GC9A01 driver, esp_lvgl_port setup;
                       idf_component.yml pins lvgl + esp_lvgl_port
  geometry/            pure C++ SafeArea content bounds; no ESP-IDF/LVGL includes
  ui/                  views (boot test screen); render a model, no logic
web/                   Web UI sources (embedded at build time)
assets/src/            high-res default images (sources)
data/                  generated LittleFS image content
test/                  native host tests + fixtures
scripts/               native build source selection (implemented)
tools/                 build scripts (web embed, asset conversion, boundary test)
```
