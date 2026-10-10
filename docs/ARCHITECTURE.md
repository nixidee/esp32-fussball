# Architecture

> Status: the C6 implementation candidate contains core, network, all four
> providers, fixed snapshot storage, device screens/input, images, embedded Web,
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
### Current ownership

- The application loop polls input, health, network policy, screen presentation
  and OTA health every 20 ms. It builds a fixed view model and calls the view
  under `display::lock()`. LVGL's own worker renders under the same recursive
  lock. Views only render supplied data; provider and navigation logic stay
  outside the view.
- HTTP image publication/deletion is the one explicit cross-task LVGL
  operation: it acquires that lock, drops a cache entry and swaps/removes a
  file after the old reader has finished. Validation and upload run outside
  the lock. No event callback calls LVGL.
- This keeps the existing app/LVGL tasks rather than adding a separate UI
  task. It replaces the earlier provisional exclusive-UI-task plan; one
  persistent scene of 17 objects is reused across modes.
- One provider worker owns all fetches, parsing, route composition and
  publication. Two snapshots and one reusable route scratch are static
  storage. Publication swaps the index under a mutex; `football::read()`
  holds that mutex while the presenter builds its bounded owned view model.
  Status readers copy only bounded status. A caller may not retain model
  references after its reader callback returns. Reader lock duration is
  still to be measured.
- Parser strings are copied into canonical fixed records before the document
  is released. Every request carries settings generation and network epoch;
  obsolete results cannot publish. Missing and estimated values retain
  explicit provenance.
- The HTTP worker performs strict settings mutations and bounded streaming
  uploads. Scan/selection/refresh requests signal their owning service.
  The static debug worker permits one fixed in-flight message. Heavy admission
  serializes provider TLS/JSON with image mutation and OTA.
- Settings live behind a mutex. `settings::current()` returns a 1,736-byte
  owned copy on the C6. NVS saves/reset use a second mutex and execute in the
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
| `provider` | 3 | 10,240 B, static | Fetch/parser/composition |
| `portal_dns` | 2 | 2,048 B, static | Captive DNS |
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
- **Notifications only:** an event says *that* something changed, never
  *what*. The payload is at most 4 B (`uint32_t`), stored inside the queue
  entry; no post allocates heap. The receiver fetches the current state with
  a short copy from its owner (for example `settings::current()`).
- **Catalog** (`event_admission.h`): state events `kSettingsChanged`,
  `kNetworkState`, `kDataUpdated`, `kOtaState`, `kTimeChanged`; the action
  event `kUiAction` with the action code as payload. Core, network, data,
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
- A settings event callback only raises an atomic flag. The app task copies
  the settings (1,736 B in this candidate), applies a changed zone and updates diagnostics;
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
  file and time status. Snapshot pause and complete report durations are
  recorded, including the final console write. Console output can delay the
  app loop; its measured duration is part of the acceptance budget.
- **Task snapshot:** two fixed tables hold at most 16 tasks (960 B together
  on the C6). The scheduler stays suspended while collecting task data and
  copying names into owned rows; formatting happens after it resumes. An
  incomplete or over-capacity scan produces no rows and logs the limit,
  never a partial or stale list. On multicore targets only copied numeric
  IDs and watermarks are exposed; names stay empty. S3 acceptance is pending.
  FreeRTOS trace is enabled for this API; CPU runtime statistics and the
  FreeRTOS text-formatting helpers remain disabled.
- **App-loop watchdog:** after startup work and boot diagnostics finish,
  `app_loop` is registered with the existing 5 s task watchdog. Each completed
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
709735 us (14 lines), below the unchanged 5 s watchdog. In normal firmware,
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
- One persistent scene is reused for all four modes, with one image and
  sixteen labels; text is only replaced when its value changes.
- LVGL draw buffers partial, double buffered, internal DMA-capable RAM:
  1/12 of the screen height each (240×240: 2 × 20 rows = 2 × 9600 B), size
  computed from the profile (`components/display/display_port.cpp`).
  LVGL heap `LV_MEM_SIZE` = 32 KB static (`.bss`), see `sdkconfig.defaults`.
  The candidate keeps these limits. Full-screen/background/font/object peaks
  remain unmeasured; the old boot measurement does not prove sufficiency.
- JSON streams through a bounded array walker. Relevant records are mapped
  one at a time; skipped fields still count toward byte, depth and allocation
  bounds. A full response is never buffered.
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

### Current compiled resource ledger — 2026-10-10

| Item | C6 candidate | Growth over the accepted core image |
|---|---|---|
| Firmware binary | 1,607,728 B | +1,091,424 B |
| PlatformIO flash report | 1,606,974 B, 87.6% of 1,835,008 B | +1,091,310 B |
| Static data/BSS | 196,736 B | +148,516 B |
| Linked DIRAM, official IDF size report | 266,066 B | +172,976 B |
| Football snapshots | 3 × 27,168 B = 81,504 B | Included above |
| Settings model / encoded record | 1,736 / 1,720 B | Format 2; legacy core import without NVS rewrite |
| UI view model | 1,416 B | Static owned view |
| Parser live allocation cap | 24,576 B including custom headers | Libc metadata additional |
| JPEG validation workspace | 4,096 B static | Included above |
| Stock JPEG files / filesystem image | 7,862 / 458,752 B | Logical files / gross partition |

DIRAM consists of 179,000 B BSS, 17,736 B data and 69,330 B RAM-resident code.
The full IDF certificate bundle is retained. No partitions, public features
or library versions were removed to fit the slot. Runtime initialization of
large snapshot defaults, shared stock aliases/font roles and unused C6 SDK
speed/protocol features reduced the first complete build's 94.3% occupancy
to the current 87.6%.

The image fits the OTA slot but misses the 85% reserve goal by about 47.2 KB
using the PlatformIO report. No flash-reserve gate is accepted. The linked
186,046 B remaining DIRAM is not measured free heap: dynamic stacks, network,
TLS, parser, DMA and library charges must still be accounted for. Peak heap,
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
  football/                  canonical model, four mappers, client/store/budget
  ui/                        pure input/navigation policy, presenter, views
  ui/fonts/                  generated fixed glyph subsets
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
