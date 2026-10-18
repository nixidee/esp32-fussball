# Architecture

Implementation checkpoint2026-10-18: configuration has one NVS owner; football
matches/tables/events/coverage are RAM-only. Adopted names are a separate
bounded collection selected by an exact reference in that one config record.
One Apply prepares names, validates the complete desired application and then
publishes one coherent revision. Settings-only Apply does not depend on files.
Two immutable names slots cover atomic activation and uncertain NVS recovery.
No legacy migration, football cache, all-team name projection or worker name
fence belongs to this design.

`components/json` owns the strict pull grammar and checked writer. Web commands,
configuration, status and exports now use typed codecs without a JSON document.
All provider sporting, selection and full-original readers use the same grammar. Typed
Web helpers own field/array/scalar validation; provider adapters own their data
semantics. Transport owns body limits, deadlines and real send admission.

The configuration owns each competition's SourceKey, separate ClubPreference,
explicit group membership and attached display preferences. A compact effective
current-phase Program is derived once for the controller. It replaces broad UI
Model/Policy copies and independently synchronized association identities.
The presenter receives facts and emits labels; renderer owns geometry/fonts;
the controller owns situation/navigation/visit timing. One action FIFO serves
physical and Web input. Software night rendering is removed; only a real PWM
backlight pin enables the night schedule.

The active C6 integration checkpoint passes; subsequent coordinated source
cutovers require another build. Host/build evidence does not establish physical
screens, heap/stack minima, OTA rollback, slow clients or real-provider behavior.
Historical measurements describe only their dated workload; the current
ownership and resource contracts below govern new changes.

## Goals
- Show football data for one club on small displays (first: 240×240 round).
- Scale to other boards, displays (360/466 px, rectangular), touch and
  additional data providers **without rewriting** screens or logic.
- Robust 24/7 operation: no hangs, bounded memory, safe updates.

## Layers

```text
Browser: Overview / Competitions / Display / Images / Settings / Debug / System
    | current typed HTTP commands and one shared draft/Apply
Web adapters ----> settings (one AppConfig / NVS) <---- runtime projections
    |                  | exact names reference
    |             adopted names (bounded LittleFS collection)
    | commands
    +----> football service / scheduler / provider adapters / shared JSON
    |          | canonical RAM store, source and dataset status
    |          +----> controller: situation / focus / programme / navigation
    |                         | owned read projections
    |                    presenters: final labels / roles / progress
    |                         | ViewModel
    +----> images        renderer / LVGL / display transfer completion
               |                    |
         checked files       geometry / hardware profiles

Independent owners: network, timekeeping, events, health/OTA (system).
main.cpp composes owners and passes narrow settings; there is no core catchall.
```

## Responsibilities and rules
| Module | Does | Does not |
|---|---|---|
| Provider | HTTP request, parse, map to canonical model, report capabilities | Decide when to poll, know about screens |
| Match store | Hold matches, tables, teams and own-match events of all competitions, detect changes (score change, final whistle) | Network I/O |
| Names | Own immutable catalogue, adopted collection, referenced files and visible label resolution | Provider requests, match-store mutation or competition grouping |
| Request scheduler / plans | Choose the next job from due times and live windows, enforce the per-provider and global limits; turn a job into provider requests | Parsing |
| Effective Program / navigation policy | Derive phase, enabled types, focus, visit/page rules and manual selection from typed facts/configuration | LVGL, provider or file I/O |
| Presenter | Build a view model for one screen from the store views + settings | Draw |
| Renderer | Reuse the persistent scene and paint supplied ViewModel through SafeArea, actual fonts and LVGL | Business logic, data access |
| Input mapper | Map raw events (button press/long, touch gesture) to `UiAction` by device config | Navigation decisions |
| UI controller | Admit typed actions, own programme/visit/navigation state and request final presentation | Input hardware, provider parsing |
| Status presenter | Supply final status labels, priorities and visibility duration to the ViewModel | LVGL, network or file I/O |

## Threading and data ownership
### Current ownership

- The application loop polls input, health, network policy, screen presentation
  and OTA health every 20 ms. It builds a fixed view model and calls the view
  under `display::lock()`. LVGL's own worker renders under the same recursive
  lock. Views only render supplied data; provider and navigation logic stay
  outside the view.
- The UI controller owns one narrow UiConfig (448 B) and one view model (2,044 B), the input
  queue and the presentation cadence. Pure navigation policy owns screen,
  row/page and competition-cycle state; separate football and status
  presenters borrow those objects during a repository callback. They retain
  no repository pointers and allocate no additional model or view copy.
- HTTP image publication/deletion acquires the cross-task LVGL
  lock, drops a cache entry and swaps/removes a
  file after the old reader has finished. Validation and upload run outside
  the lock. Screenshot admission also takes that lock briefly to invalidate
  the active screen. A permanent flush wrapper copies before byte swapping,
  then hands one owned strip to the HTTP caller using bounded ready/ACK
  ownership. The consumer never waits under the LVGL lock. The producer
  retains the existing task's LVGL lock during its coherent redraw and can
  wait only until the capture deadline. Cancellation detaches the active
  pointer and joins only capture copying/ACK work, before the original SPI
  callback. No event callback calls LVGL.
- This keeps the existing app/LVGL tasks rather than adding a separate UI
  task. It replaces the earlier provisional exclusive-UI-task plan. Every
  mode reuses two persistent child objects (one image and one draw object)
  under the active LVGL screen. The fixed application draw list and text
  arena do not replace LVGL's temporary draw-task allocations.
- One provider worker owns all fetches, parsing, the match store and
  publication. The store (46,928 B in the current C6 compiler layout,
  512 matches/256 teams) is one static object; the worker writes
  each response element into it under a mutex and publishes after every
  request. `football::read()` holds that mutex while the presenter builds
  its bounded owned view model. Football data is RAM-only and is reacquired
  after boot; only adopted names have their separate LittleFS owner. Operational
  request credits remain in NVS. The current-only 36-byte discovery guard stores
  two counters and an active-run marker; provider traversal and source/access
  fingerprints remain in RAM. Reboot restarts traversal with remaining durable
  credits, and midnight cannot replenish an active run. A provider request never
  waits inside a held admission token for a limit: the worker asks the request scheduler what may run,
  reports each request's start and outcome to it, and sleeps until the
  scheduler's next wake time. The owner's selection list is a pending work
  item served one request per pass.
  Status readers copy only bounded status. A caller may not retain model
  references after its reader callback returns. Reader lock duration is
  still to be measured.
- Parser strings are copied into the store's fixed records before the
  element is released. Every request carries the provider-data generation of the
  settings and the network epoch; obsolete results cannot publish. Its first
  send also checks the matching settings generation, under the existing settings
  writer, so a policy or request-limit edit cannot admit a stale unsent request.
  This includes setup queries; they bypass only the frame pause. After admission,
  policy-only changes retain RAM facts and permit the started response to finish.
  Missing and estimated values retain explicit provenance.
- The HTTP worker performs strict settings mutations and bounded streaming
  uploads. Scan/selection/refresh requests signal their owning service.
  The static debug worker permits one fixed in-flight message. Heavy admission
  serializes provider TLS/JSON with image mutation, screenshots and OTA.
- One application configuration owns general settings, competitions, screen
  policy and the exact adopted-names reference behind one model mutex. Section
  readers copy only their requested projection. The complete snapshot includes
  its revision; one writer mutex serializes candidate validation and one NVS
  envelope. Verified durable publication advances the generation before
  unlocking and posts the notification afterward. An uncertain NVS mutation
  retains the previous coherent RAM snapshot and blocks further writes until
  recovery/reboot. A complete candidate and the wire buffer are transient heap
  allocations, not additional resident configuration owners. Saves/reset run in
  the caller task and post coalesced `kSettingsChanged` notifications.
- State notifications use the bounded event bus. Semantic input actions use
  only the controller's direct four-entry FIFO, which
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

### Event bus and UI commands

`components/events/event_bus.*` owns a separate `esp_event` task/loop for five
state notifications: settings, network, canonical data, OTA and time. Each kind
has one guaranteed queue slot and coalesces while pending. The dispatcher clears
its pending mark before invoking subscribers; a change during delivery queues
another notification. Events carry no data. Subscribers read the current owner
state, remain short and never call LVGL or do blocking work. The fixed eight-row
subscriber table and2304B task stack remain. Failed posts are counted and logged.

UI commands enter `ui::submit(Action, expected_revision)` directly. Physical
inputs and HTTP share its one fixed four-slot FIFO. Each command retains the
complete32-bit configuration revision through admission and consumption; stale
commands cannot affect a newer configuration. Full admission returns timeout;
there is no event-bus forwarding queue, UI event kind, event payload or second
UI-slot reservation. The state-event queue shrinks from nine to five entries:
at the configured20B SDK entry size this saves80B of queue storage, before heap
allocator charges. Current static/heap/task reserves still require target and
device measurement; older bus measurements do not establish those reserves.

The optional `EventTest::kFlood` boot check posts20 settings notifications while
a test subscriber sleeps50ms. It expects successful posts with coalesced
notification delivery and prints the counters. This updated device scenario has
not been run on owner hardware. Pure host checks prove valid kinds, coalescing,
release, independent state notifications, queue capacity and invalid-kind bounds.

## File service (implemented, device-tested)
`components/files/file_service.*` (namespace `files`); name rules, path
building and the erased-flash check are the pure component `components/files`
(host-tested). Library: `joltwallet/littlefs` (version pinned in
`components/files/idf_component.yml`).

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
  Upload and role copy share checked descriptor I/O, synchronization/close,
  actual-byte JPEG validation and final rename. A failed candidate never
  replaces the published file. One replacement workspace is admitted using
  actual available space, with no second reserve charge; filesystem allocation
  errors are still handled. A four-byte validation bitmap avoids another full
  JPEG decode on unchanged visits and is invalidated by mutation or an observed
  decoder failure. Copy uses a separate 512-byte stack frame that returns before
  JPEG validation; it adds no whole-image buffer. Delete receipts distinguish
  committed reference cleanup, actual unlink and an uncertain settings write.
  No startup legacy-image maintenance remains.
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

`components/timekeeping/time_service.*` owns wall-clock validity and the global
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
- A settings event callback only raises an atomic flag. Main reads an immutable
  configured-zone reference and the four-byte diagnostic preferences, then
  passes them to their owners outside the `events` task. Time retains the
  requested zone for allocation retries without querying settings itself.
- Civil windows follow local time, including midnight and skipped/repeated
  daylight-saving hours. Delays and deadlines use monotonic time.
- The C6 offline test passed 112 zone-rule cases, 12 real-clock window cases,
  backward/forward jumps and notification/validity checks. Details and test
  selector: [NETWORK.md](NETWORK.md#time). SNTP wiring and hardware-pin night
  scheduling are implemented; their network/device acceptance is pending.

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

`components/system/health_service.*` runs in the existing app task; the pure
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
console hook or retained history; see [WEB_UI.md](WEB_UI.md#system-and-diagnostics).
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
  Avoidable general-heap allocation churn remains prohibited. Typed provider
  workspaces, TLS and screen transitions still need peak and
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
- One strict pull JSON grammar validates complete documents, including skipped
  fields, UTF-8, number delimiters and nesting. Typed consumers stage only
  facts that require unordered-field resolution; there is no DOM or second
  array-walker grammar. Transport byte limits are independent of workspace
  sizes. Season selection uses a transient compact round/own-match index and
  a second streamed pass, rather than a full season or whole-store undo copy.
- One common checked little-endian cursor, CRC32 and UTF-8/value helpers serve
  durable codecs and identities; providers keep their own semantic rules.
- TLS: one connection at a time.

### Staged resource acceptance
The active C6 target is accepted in stages: baseline integration (WiFi/TLS,
streaming JSON, JPEG and HTTP server), complete data model and production views,
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

## Scalability rules
- No absolute pixel values in views: positions/sizes from layout tokens
  derived from resolution, shape and size class.
- Round content must pass geometric safe-area tests; rendered visibility
  needs owner-flashed device screenshots. Backgrounds, clipping containers, decoration and
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
  display/                   panel/LVGL port, JPEG validator, rotation/PWM
  events/                    state notifications and portable admission
  files/                     checked I/O, paths, LittleFS mount/format
  geometry/                  portable SafeArea
  settings/                  AppConfig, pure codecs/projections, NVS Apply
  timekeeping/               civil rules and applied clock/zone
  system/                    local health/watchdog diagnostics and OTA
  network/                   WiFi policy, antenna, DNS, mDNS, SNTP, admission
  football/                  compact match store + row mappers, request
                             scheduler, provider request plans, matchday
                             rules (gap, over, passed, selection), views from
                             the store, provider worker, client, budget and
                             temporary original-name query channel
  names/                     immutable catalogue, adopted collection/JSON/file
                             codecs, atomic names/config coordination and
                             visible label resolution
  ui/                        controller, pure input/navigation policy,
                             football/status presenters, views
  ui/fonts/                  generated fixed glyph subsets
  ui/lvgl_font/              LVGL default-font header (14 px text role)
  images/                    quotas, validation, cache lifetime and publication
  web/                       strict settings/API, upload and live debug
web/                         offline browser sources
assets/src/ assets/fonts/     stock artwork and licensed font sources
data/                        generated stock LittleFS content
test/                        portable owner-behaviour and regression suites
scripts/                     configuration guards, native selection, heap patch,
                             formatting, asset/font/web builders, release guard
```


## Current configuration and codec resource ownership

Current ownership supersedes earlier transitional ledger entries above. There
is no `components/core`: settings owns NVS, files owns checked LittleFS I/O,
events owns notifications, timekeeping owns clock state, and system owns health
and OTA. The component graph is acyclic. Runtime consumers copy only their
projection: WebSettings131 B, NetworkSettings291 B, ProviderSettings284 B and
UiConfig448 B, versus full general Model704 B. These are copy/frame reductions,
not automatically static-RAM savings or proof of free stack.

AppConfig is2704 B; its single exact-current record is at most2618 B, including
693 B general, up to1678 B competitions,216 B policy and aggregate metadata.
The old standalone records and compatibility repair entrypoints are retired.
Shared binary helpers preserve the current encoded bytes. The only names
activation boundary is common Apply, with exact reference and uncertain-write
read-only recovery; the former impossible pre-write admission fence, full-team
projection and worker name fence are superseded. Checked allocation/write/close/
validation/publish handles physical failures without a universal filesystem
proof. The unchanged40-KiB replacement workspace is shared, not doubled.

All JSON consumers use the common strict reader/writer. Fixed HTTP chunk/read
buffers remain512/128 B; common Apply may allocate one2704-B candidate and one
bounded names-import arena when names change. There is no 24-KiB JSON DOM.
Per-dataset status copies add576 B transiently to `/status`; they preserve table
and details failures independently of successful fixtures. The final matching
C6 build measures1,732,486 B linked Flash and174,200 B static RAM. Binary
1,733,248 B and Webgzip89,818 B match the same source manifest; the earlier
checkpoints describe only their own snapshots. Against the preserved baseline:
−33,812 B linked Flash, +13,188 B static RAM, −33,808 B binary, +9,631 B gzip.
The required canonical store is46,928 B versus33,784 B (+13,144 B); all other
static changes net+44 B. Initialized-data moves only save Flash, not total RAM.

The unchanged OTA slot leaves101,760 B free; the85%target still fails by
173,492 B and has not been waived. Provider typed workspace8064 B plus mutually
exclusive index3136 B or event7680 B requests11200/15744 B before TLS/SDK.
Names Apply requests2704+32880 B before releasing the arena ahead of the2618-B
NVS wire allocation. Export grouping is request-only and below16KiB. Heavy
admission serializes these paths with image validation/screenshot/OTA; ordinary
status and normal LVGL JPEG can overlap TLS. No allocation cap is a measured
peak. Physical TLS/JPEG/heap/largest-block/all-task stack/CPU/OTA acceptance is
still required; the85%Flash and physical gates remain open.
