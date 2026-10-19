# esp32-fussball

Football information for **one club** on small (round) ESP32 displays: live
scores, conference view of all running matches, league table and club crest.
Configured through a built-in web interface.

The current implementation candidate is **0.1.0-dev**. It includes four provider
adapters, six screen types with one controller, German/English Web UI, image
management, device screenshots, diagnostics and firmware OTA/rollback.
The source cutovers and scoped ownership/retirement review are complete for
the approved consolidation. Current keyed-provider, physical device and resource
acceptance remain open. This is not an accepted release. S3 development remains
paused.

The current architecture has one NVS application configuration and one common
Apply for settings, competitions, explicit club groups, screen programmes and
adopted names. Football data is RAM-only and is reacquired after boot. Provider
adapters and Web commands share one strict streaming JSON reader/writer.
Software night effects have been removed; night scheduling is offered only
when the hardware has a controllable backlight pin.

Current C6 quality-remediation checkpoint (2026-10-18): **1,751,182 B linked
flash, 178,740 B static RAM, 1,751,936 B binary, 92,683 B Web gzip**.
All nine source remedies are implemented and their focused integration checks pass;
current keyed-provider and physical acceptance remain open. Against the
pre-consolidation baseline this is -15,116 / +17,728 / -15,120 / +12,496 B.
The canonical MatchStore is48,576 B, including16exact resolved sources and
8independent configured views; see [data model](docs/DATA_MODEL.md#42-store-parts).
Football data is reacquired into RAM after boot.

The binary leaves **83,072 B** in the unchanged1,835,008-B OTA slot; it exceeds
the standard85% reserve limit by **192,180 B**. On2026-10-18 the owner explicitly
suspended that criterion for this alpha and its current functionality/budget.
This is an acceptance exception; the numeric occupancy still exceeds85%.
Device, runtime and OTA/rollback acceptance remain open, with no agent upload
authorized. Details: [alpha reserve decision](docs/DECISIONS.md#adr-045--alpha-only-ota-reserve-exception--accepted-2026-10-18).
These build figures do not establish runtime heap/stack reserves.

The build-generator cleanup was verified against that candidate on2026-10-18:
RAM, Flash, binary and Web gzip deltas are all0B; generated assets and firmware
remain byte-identical. The short owner device log reaches272.6s after boot:
reported lifetime heap minimum77,072B, lowest sampled largest free block86,016B,
LVGL peak8,956B and filesystem use126,976/458,752B at the last sample.
Two image uploads were refused with `409 busy`; a completed upload is not shown.
Usable task-stack sizes and worst-case/long-run measurements remain required.

Focused host checks cover actual parser responses, complete/partial acquisition,
configuration and names failures, coherent reboot, browser draft races, file
faults and screen layout calculations. Local browser checks use controlled
non-secret HTTP fixtures and the real bundled page. Physical LCD output,
maximum-load heap/stack, long-run operation and OTA trial/rollback still require
owner-flashed hardware evidence. Device screenshots are the screen-acceptance
path; there is no host LCD renderer.

The maintained [club catalogue](include/team_names.json) preserves all147 supplied
clubs with German/English/short names, aliases and verified provider ID lists.
The running firmware now embeds the complete immutable source and serves it
through `GET /api/v1/team-names/preset`, with its source SHA256 in a response
header. The full collection editor/import/export uses the common application
draft and one Apply boundary. Adopted DE/EN/short values, all provider bindings,
aliases, league metadata and ID exceptions share one bounded16-KiB collection.
NVS settings select its exact immutable names version; an uncertain write
preserves both versions and blocks mutations until recovery/reboot. There is
no legacy migration, per-row immediate save, football cache or original-name
archive. Source version1 retains full texts; adopted version2 exports each
unique text once and uses local references. Only current visible adopted fields
are cached. Provider fallback26/3 remains volatile. Physical device, provider
and maximum-load acceptance are separate from host/build checks.

## Command reference

Run all commands in the project folder. Replace `<env>` with one of the
environments below.

| Environment (`<env>`) | Hardware | Status |
|---|---|---|
| `xiao_esp32c6_gc9a01` | Seeed XIAO ESP32-C6 + 1.28" GC9A01 round display | current implementation candidate compiled; earlier boot/display/input acceptance only; current device acceptance pending; default env |
| `waveshare_esp32s3_lcd128` | Waveshare ESP32-S3-LCD-1.28 | paused; last build and boot log before the display code (P1.2); not built with the display code yet; if the first log lines are missing after upload, press RST |
| `native` | Host computer, unit tests only | pure logic and actual typed codecs; scoped suites and sanitised integration replays |

| Task | Command |
|---|---|
| Build firmware | `pio run -e <env>` |
| Check incremental build publication and failed-write preservation | `python3 scripts/tests/test_generated_file.py` |
| Build + upload firmware (USB) | `pio run -e <env> -t upload` |
| Build filesystem image (images) | `pio run -e <env> -t buildfs` |
| Upload filesystem image (USB) | `pio run -e <env> -t uploadfs` |
| Serial monitor | `pio device monitor -e <env>` |
| Upload + monitor | `pio run -e <env> -t upload -t monitor` |
| Erase flash completely | `pio run -e <env> -t erase` |
| Clean build files | `pio run -e <env> -t clean` |
| Run host unit tests | `pio test -e native` |
| Run only the SafeArea suite | `pio test -e native -f test_safe_area` |
| Run only the settings suite | `pio test -e native -f test_settings` |
| Run only screen-policy model/codec checks | `pio test -e native -f test_screen_policy` |
| Run classification and sequence checks | `pio test -e native -f test_screen_selection -f test_screen_sequence -f test_screen_eligibility` |
| Run canonical coverage checks | `pio test -e native -f test_provider_coverage -f test_schedule_coverage -f test_coverage_persistence` |
| Replay actual typed provider adapters and transport | `python3 scripts/tests/test_provider_adapters.py` and `python3 scripts/tests/test_provider_transport.py` |
| Replay shared-source planning against a real recorded season | `python3 scripts/tests/test_source_planning.py` |
| Check exact-season coverage identity and invalidation | `python3 scripts/tests/test_source_coverage.py` |
| Replay actual shared-source discovery and request credits | `python3 scripts/tests/test_source_coverage_worker.py` |
| Check shared-season Store, view ownership and stale IDs | `python3 scripts/tests/test_source_store.py` |
| Check two-pass fixture retention against real season responses | `python3 scripts/tests/test_fixture_retention.py` |
| Check common strict JSON grammar and checked binary primitives | `python3 scripts/tests/test_json_stream.py` and `python3 scripts/tests/test_binary_codec.py` |
| Check display-command HTTP/FIFO admission and input revision changes | `python3 scripts/tests/test_display_commands.py` |
| Check hardware brightness receipts and repeated-duty suppression | `python3 scripts/tests/test_backlight_pwm.py` |
| Check every lossless font bitmap and metric through LVGL | `python3 scripts/tests/test_font_bitmaps.py` |
| Check actual LVGL colour-test geometry without drawing pixels | `python3 scripts/tests/test_boot_geometry.py` |
| Check the SDK INFO filter without the unused tag cache | `python3 scripts/tests/test_log_filter.py` |
| Run live-window scheduling checks, including unknown kickoff and suspension | `pio test -e native -f test_live_window` |
| Run screen API and image-reference checks | `pio test -e native -f test_screen_json -f test_screen_images` |
| Run selected ordinary details, Demo and event visibility checks | `pio test -e native -f test_ordinary_details -f test_demo_scenarios -f test_event_visibility` |
| Run only the event admission suite | `pio test -e native -f test_events` |
| Check shared file candidates and actual image/JPEG fault paths | `python3 scripts/tests/test_file_candidate.py` and `python3 scripts/tests/test_image_files.py` |
| Run only the file name suite | `pio test -e native -f test_files` |
| Run only the timekeeping suite | `pio test -e native -f test_timekeeping` |
| Run only the heap-meter suite | `pio test -e native -f test_health` |
| Check capacity/unknown/overlap scheduler windows against an independent oracle | `python3 scripts/tests/test_scheduler_activity.py` |
| Check manual wizard IDs and every canonical phase against current C++ | `node scripts/tests/test_web_identity_phase.cjs` |
| Run browser logic regression tests on the host (Node.js) | `node scripts/tests/test_web_ui.cjs` |
| Check Debug/Overview/Demo controls against current C++ fixtures | `node scripts/tests/test_web_debug_controls.cjs` |
| Run screenshot protocol/client checks on the host (Node.js) | `node scripts/tests/test_web_screenshot.cjs` |
| Run image-editor loading, positioning and race checks | `node scripts/tests/test_web_image_editor.cjs` |
| Check pure Web draft actions and private feature scope | `node scripts/tests/test_web_draft_actions.cjs` |
| Check coherent application draft/save and complete names collection | `node scripts/tests/test_web_common_apply.cjs` |
| Run full-name dialog close/race and inherit/empty checks | `node scripts/tests/test_web_names_close.cjs` |
| Run confirmed factory-reset browser flow | `node scripts/tests/test_web_factory_reset.cjs` |
| Check complete firmware name source and deterministic generation | `python3 scripts/tests/test_team_name_catalogue.py` |
| Check bounded adopted-name codec with sanitizers | `python3 scripts/tests/test_team_name_store.py --sanitize` |
| Check full source/shared-text names import/export and POSIX candidates | `python3 scripts/tests/test_team_name_json.py` |
| Check effective names and read failures | `python3 scripts/tests/test_effective_names.py` |
| Check common config/names commit and uncertain NVS recovery | `python3 scripts/tests/test_names_config_apply.py` |
| Check the current aggregate record and coherent NVS owner under faults | `python3 scripts/tests/test_app_config_owner.py` |
| Check current provider/global counters through the actual NVS adapter | `python3 scripts/tests/test_request_budget_owner.py` |
| Check actual preset HTTP handler, cancellation and retry | `python3 scripts/tests/test_team_names_http.py` |
| Check names/backup HTTP export preflight, OOM and partial I/O | `python3 scripts/tests/test_web_names_export.py` |
| Run screen-policy draft/save/import checks on the host (Node.js) | `node scripts/tests/test_web_screen_policy.cjs` |
| Check accessible save feedback and atomic import/export flows | `node scripts/tests/test_web_save_feedback.cjs` |
| Emit current production schema/config fixtures for controlled client checks | `python3 scripts/tests/web_config_fixture.py` |
| Check the device-checker's current Apply and HTTP load without device access | `python3 scripts/tests/test_device_check_config.py` |
| Check original-name stream correlation and backpressure | `python3 scripts/tests/test_original_names_channel.py` |
| Check original-name worker, encoder and actual provider parser integration | `python3 scripts/tests/test_original_names_integration.py` |
| Check original-name HTTP completion, disconnect and partial failures | `python3 scripts/tests/test_original_names_http.py` |
| Check actual HTTP/WebSocket partial-send deadlines, debug ownership and file completion | `python3 scripts/tests/test_web_transport.py` |
| Check selector receipts, stale IDs, result replacement and strict cancel HTTP | `python3 scripts/tests/test_selection_query_http.py` |
| Check correlated wizard results, closing, cancellation and lost replies | `node scripts/tests/test_web_selection.cjs` |
| Set up host tools (once) | see “Code formatting” below |
| Check formatting of changed lines | `.venv-tools/bin/python scripts/check_format.py` |

Upload, erase, filesystem and monitor commands apply to firmware environments.
Browser acceptance uses the real embedded page after the owner uploads the
firmware. Resource acceptance uses the existing health log; it is separate
from a successful build. The device-check script uses the current configuration revision/Apply contract;
its offline command checks do not establish device acceptance.
Native tests use Native 1.2.1 and Unity's exact `v2.7.0` Git tag, checked against
official releases on 2026-10-09. Unity's tag retains stale 2.6.0 package metadata,
so PlatformIO may display that version and download an unused registry copy;
the suite checks that the compiled header is actually 2.7.0. Other host operating
systems are not yet verified.

Firmware updates through Web UI → System are implemented. The first
candidate install uses USB; later compatible OTA uploads preserve LittleFS.
Actual upload, trial health and rollback acceptance remain pending. USB `uploadfs` is for
initial setup/development: it replaces the filesystem image and may erase
uploaded user images; no preservation workflow is provided.

Build configuration files:
- `platformio.ini` — one environment per hardware target.
- `sdkconfig.defaults` (all targets) and `targets/<target>.sdkconfig.defaults`
  (board-dependent: flash size, PSRAM, console, partition file) — ESP-IDF
  settings. The generated `sdkconfig.<env>` is a git-ignored build artefact.
  `scripts/sdkconfig_guard.py` regenerates it when a configuration input
  changes (defaults files, partition CSV, component manifests, lockfile,
  platform/ESP-IDF version). `scripts/sdkconfig_verify.py` then stops the
  build if any setting of the defaults files is missing from or differs in
  the generated file, uses a deprecated option name, or if
  `board_build.partitions` differs from `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`.
  Settings changed in `menuconfig` are temporary: put permanent ones into a
  defaults file. After a failed check the next build regenerates the file.
- `partitions/*.csv` — flash layout per flash size.
- `components/*/idf_component.yml` — exact versions of external ESP-IDF
  components (LVGL, esp_lvgl_port, LittleFS and mDNS), resolved into one lockfile per chip target,
  `dependencies.lock.<chip>` (e.g. `dependencies.lock.esp32c6`), set in the
  root `CMakeLists.txt`. Lockfiles are tracked. The component manager changes
  a lockfile only when a manifest changes; review and commit both together.
  The first build of a new chip target creates its lockfile.
- `scripts/native_sources.py` — explicit pure-component selection for host
  tests; firmware and tests compile the same geometry, settings, events,
  files, timekeeping and heap-meter sources without copies.
- `scripts/heap_monitor_patch.py` / `.cmake` — apply the project's bounded
  heap-monitor allocation-failure correction to a build-local ESP-IDF source
  copy. The shared SDK stays unchanged; an unexpected SDK version or source
  hash stops the build and requires review.
- `.clang-format`, `requirements-tools.txt`, `scripts/check_format.py` — code
  style and its check (see “Code formatting”).
- `boards/`, `displays/`, `targets/<target>.h` — hardware profiles (pins,
  display, inputs); the env's `build_flags` selects the target. Wrong pin
  assignments fail the build. Details: [docs/HARDWARE.md](docs/HARDWARE.md).

### Code formatting

C/C++ code follows `.clang-format` (Google style, 80 columns). The check
covers only lines changed against a Git commit (default `HEAD`, i.e.
uncommitted changes); existing code is reformatted only when it is touched.
It never rewrites files. New files are checked once Git knows them
(`git add`, or `git add -N`).

The pinned clang-format version (`requirements-tools.txt`) is installed into a
project-local, git-ignored Python environment `.venv-tools/`, created once with
the Python that comes with PlatformIO (macOS/Linux paths; on Windows use
`.venv-tools\Scripts\python.exe`):

```bash
~/.platformio/penv/bin/python -m venv .venv-tools
```

```bash
.venv-tools/bin/python -m pip install -r requirements-tools.txt
```

```bash
.venv-tools/bin/python scripts/check_format.py
```

Exit code 0 means formatted, 1 means deviations (printed as a diff), 2 means a
tool setup problem (wrong Python or clang-format version). Pass a commit as
argument to check a range, e.g. `scripts/check_format.py main`.

---

## Development setup
1. Install [PlatformIO](https://platformio.org/install) (VS Code extension or CLI).
2. Optional: copy `include/secrets.h.example` to `include/secrets.h` and fill
   in your WiFi, club and API keys (the template marks which keys are used
   today; the WiFi keys are `SECRET_WIFI_SSID` / `SECRET_WIFI_PASSWORD`).
   Without it the firmware builds with defaults and everything is set up in
   the Web UI. See
   [docs/CONFIGURATION.md](docs/CONFIGURATION.md).
3. Connect the board via USB.
4. For initial setup/development, upload firmware and filesystem:
   `pio run -e <env> -t upload` and `pio run -e <env> -t uploadfs`.
   The filesystem step replaces its contents, including uploaded user images.
   Routine compatible updates use firmware OTA without a filesystem upload.
5. Without WiFi credentials the device opens the setup network
   `Fussball-` followed by six hexadecimal characters. Connect with your phone
   and browse to `http://192.168.4.1`; automatic captive opening depends on
   the phone and is not yet accepted.
6. With WiFi the device shows its IP address for 60 s. Open it in a browser
   (or `http://fussball.local`).

## Development candidate and resources

The C6 candidate uses the existing 4 MB dual-OTA layout, a fixed 32 KB LVGL
pool and two 9,600 B DMA draw buffers. One fixed match store holds the
football data of all competitions in 48,576 B (512 matches, 256 teams; C6
compiler layout). Typed parsers stream full responses and retain one bounded
response workspace, rather than a JSON document. Season selection uses a
transient compact index and admits the required round/own-match union; it does
not load an entire season into the store. Shared admission prevents
simultaneous provider TLS/JSON, image mutation, screenshot capture and OTA. Normal JPEG redraw
can overlap provider traffic and still needs peak-memory measurement.
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) records ownership and the current
linker budget.

**Debug → Bildschirmfoto herunterladen** saves a round PNG of
the next real device redraw, with transparent corners and a dated filename.
It uses the existing login policy and keeps the current reading orientation.
The C6 temporarily copies one 9,600 B strip; the browser makes the PNG.
The display may pause for up to five seconds. Physical panel/output and
runtime reserve acceptance remain pending after the owner flashes the build.

The stock filesystem contains two baseline JPEG aliases (7,862 logical bytes)
for the crest/slideshow and screen backgrounds. These factory images stay on
the device; the Web UI can replace the crest and the standard background and
restore the factory images at any time. Rebuild from the high-resolution
sources when a target resolution changes:

```bash
python scripts/build_assets.py --target xiao_esp32c6_gc9a01
pio run -e xiao_esp32c6_gc9a01 -t buildfs
```

The asset/font builders require Pillow 12.3.0; ordinary firmware builds use
the committed generated fonts and need no Pillow. Font source/licence are in
`assets/fonts/`; stock artwork provenance is in `assets/src/README.md`.
Regenerate the lossless 4-bpp fonts with `python3 scripts/build_fonts.py` using
an interpreter with that pinned Pillow already installed. The builder checks
the original source SHA-256; `python3 scripts/tests/test_font_bitmaps.py`
compares every decoded glyph and metric through the actual LVGL reader.
The gzip Web UI header is regenerated by the PlatformIO pre-build script,
including cached CMake builds; unchanged content does not recompile it.

`VERSION` is the application version source; [CHANGELOG.md](CHANGELOG.md)
describes the candidate. CMake option `FUSSBALL_RELEASE_BUILD=ON` additionally
requires a plain semantic release version and all seven diagnostic/fault selectors
disabled. It does not certify runtime acceptance or publish anything.

Waveshare verification resumes after C6 acceptance. Other resolutions, controllers
and inputs still require their own buffer, font and hardware acceptance.
The C6 antenna driver is implemented; RF switching has not been tested.

## What it shows

The controller uses one ordered programme for each situation: own match, other
matches, matchday, idle and optional season break. Six screen types participate:

| Screen | Content |
|---|---|
| Single | Focused own or unique ordinary match, principal score, phase/minute, events, real reported shootout attempts; held result or next preview when applicable |
| Conference | Applicable matches within one competition, optional own pin, balanced pages and complete manual paging |
| Matchday | The selected sporting round, including rescheduled fixtures; previous/current/next navigation and competition stepping |
| Table | Provider groups remain distinct; automatic own-group/club centring and complete manual groups/rows, with played/difference/points retained |
| Crest | Dedicated crest replacement or factory image |
| Images | Library image sequence, continuing after the last actually displayed image |

The common type order, automatic/manual participation and visit durations are
configurable. Competition-specific values inherit field by field; returning to
shared values preserves dormant overrides. Explicit club groups are shown in
priority order and determine focus; equal names do not link competitions.
Frame mode shows the configured images/crest. Buttons navigate and return to
automatic mode according to their assignments; Web commands use the same FIFO.
Software night effects are removed. Rotation, IP display, normal colours and
backgrounds remain configurable; scheduled night brightness needs a real
backlight pin. These controls are implemented; physical display/input and
resource acceptance still need owner-flashed hardware. Details: [UI.md](docs/UI.md).

## Hardware
Pinouts and wiring: [docs/HARDWARE.md](docs/HARDWARE.md).

| Board | Display | Notes |
|---|---|---|
| Seeed XIAO ESP32-C6 | 1.28" GC9A01 240×240 round (external, no backlight pin) | earlier display/touch acceptance; candidate untested; 3 touch modules; optional external antenna |
| Waveshare ESP32-S3-LCD-1.28 | built-in 240×240 round | 16 MB flash, 2 MB PSRAM; no inputs (BOOT button not reachable in the housing) |
| Seeed XIAO ESP32-S3 | 1.28" GC9A01 240×240 round (external) | later |

## Data sources
Four adapters are implemented: **OpenLigaDB** (default, no key), API-Football,
ESPN (unofficial, opt-in) and football-data.org. Keyed providers require owner
credentials and tier/coverage acceptance. Up to eight competitions, each with
exactly one source, are stored as a list with a fixed maximum; providers are
not combined.
Provider coverage, limits and endpoints:
[docs/DATA_PROVIDERS.md](docs/DATA_PROVIDERS.md); what the firmware requests
and how it maps and stores the data: [docs/DATA_FLOW.md](docs/DATA_FLOW.md);
the stored data and where it is shown: [docs/DATA_MODEL.md](docs/DATA_MODEL.md).

## Documentation
| File | Content |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, modules, threading, memory, source layout |
| [docs/HARDWARE.md](docs/HARDWARE.md) | Boards, displays, pinouts, profile system |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Config file, `secrets.h`, settings, factory reset |
| [docs/UI.md](docs/UI.md) | Screens, navigation, round display rules, overlays |
| [docs/NETWORK.md](docs/NETWORK.md) | WiFi behaviour, setup AP, time service, web server, OTA |
| [docs/WEB_UI.md](docs/WEB_UI.md) | Web interface tabs, image editor |
| [docs/API.md](docs/API.md) | Device REST API: endpoints, status fields, sessions, error codes |
| [docs/DATA_MODEL.md](docs/DATA_MODEL.md) | Data sets, definitions, fields per provider, usage per screen, capacities, gaps |
| [docs/DATA_FLOW.md](docs/DATA_FLOW.md) | Requests, filtering, mapping, RAM retention, publication, request bounds |
| [docs/DATA_PROVIDERS.md](docs/DATA_PROVIDERS.md) | Provider coverage, limits, conditions, endpoints, research |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Architecture decisions (ADRs) |
