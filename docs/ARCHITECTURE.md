# Architecture

> Status: **partly implemented** (P1): hardware profiles, display port
> (`components/display`), boot test screen (`components/ui`),
> `include/app_config.h` (debug group). Everything else is planned.
> Decisions in [DECISIONS.md](DECISIONS.md); items marked “proposed” wait
> for approval.

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

## Threading (proposed, finalised in P2.7)
- **UI task**: owns LVGL exclusively. Other tasks never call LVGL; they post
  events. Views update only in this task.
- **Network/data task(s)**: HTTP requests and parsing.
- **HTTP server task**: from `esp_http_server`; hands work to other tasks via
  events/queues for anything slow.
- Communication via event bus (`esp_event`) and immutable snapshot handover
  (double buffer) — no shared mutable model between tasks.
- **Current state (P1.4, interim):** LVGL runs in the `esp_lvgl_port` task
  (priority 4, stack 7168 B, port defaults). The boot test screen is created
  once from `app_main` while holding `display::lock()` (recursive mutex of
  `esp_lvgl_port`). This deviates from the rule above and is decided in P2.7.

## Memory strategy (C6 has no PSRAM)
- Fixed-capacity model containers, allocated once.
- One screen alive at a time; LVGL objects of the previous screen are deleted.
- LVGL draw buffers partial, double buffered, internal DMA-capable RAM:
  1/12 of the screen height each (240×240: 2 × 20 rows = 2 × 9600 B), size
  computed from the profile (`components/display/display_port.cpp`).
  LVGL heap `LV_MEM_SIZE` = 32 KB static (`.bss`), see `sdkconfig.defaults`.
- JSON parsed directly from the HTTP stream with a field filter (ADR-009);
  the full response is never buffered.
- TLS: one connection at a time.

## Scalability rules
- No absolute pixel values in views: positions/sizes from layout tokens
  derived from resolution, shape and size class.
- Every screen must pass the round-boundary test on every round profile.
- Input-agnostic navigation: screens react only to `UiAction`s.
- New provider = new module implementing the provider interface + fixtures +
  capability flags; no change in presenters or views.
- New display/board = new header files in the profile folders + one env.

## Source layout (proposed, final in P1)
```
platformio.ini
sdkconfig.defaults     ESP-IDF settings shared by all targets
partitions/            partition tables per flash size
boards/ displays/ targets/   hardware profiles; targets/<target>.sdkconfig.defaults
                       = board-dependent ESP-IDF settings (ADR-010)
include/hw_profile.h   hardware profile types + compile-time pin checks
include/hw_target.h    selects the target (build flag), runs the checks
include/app_config.h   single software configuration (defaults, limits);
                       exists since P1.4 with the debug group, rest in P2.1
include/secrets.h      local secrets + personal presets (git-ignored)
src/                   entry point, app wiring
components/            ESP-IDF components = modules (core, net, web, data,
                       providers/*, ui, input). Existing (P1.4):
  display/             SPI bus, panel IO, GC9A01 driver, esp_lvgl_port setup;
                       idf_component.yml pins lvgl + esp_lvgl_port
  ui/                  views (boot test screen); render a model, no logic
web/                   Web UI sources (embedded at build time)
assets/src/            high-res default images (sources)
data/                  generated LittleFS image content
test/                  native host tests + fixtures
tools/                 build scripts (web embed, asset conversion, boundary test)
```
