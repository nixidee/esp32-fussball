# esp32-fussball

Football information for **one club** on small (round) ESP32 displays: live
scores, conference view of all running matches, league table and club crest.
Configured through a built-in web interface.

> **Project status: implementation candidate, 0.1.0-dev.** WiFi/setup AP,
> all four data adapters, bounded routing, four device screens, navigation,
> images, the German/English Web UI, live debug and OTA/rollback are implemented.
> The active C6 firmware and stock filesystem image build successfully.
> Device acceptance is pending; provider fixture, browser, maximum-load and
> long-run tests remain. Earlier boot/core tests
> do not establish acceptance of these new services.
>
> Current C6 size: 1,607,728 B firmware binary, 1,606,974 B PlatformIO flash
> (87.6% of one OTA slot), 196,736 B static RAM and 266,066 B linked DIRAM.
> It fits the slot but exceeds the agreed 85% flash-reserve target by about
> 47.2 KB. Peak heap, largest block, stack and LVGL reserves remain unmeasured
> for the complete candidate. Visual/resource polish and acceptance are next;
> this is not a release. S3 development remains paused.

---

## Command reference

Run all commands in the project folder. Replace `<env>` with one of the
environments below.

| Environment (`<env>`) | Hardware | Status |
|---|---|---|
| `xiao_esp32c6_gc9a01` | Seeed XIAO ESP32-C6 + 1.28" GC9A01 round display | current implementation candidate compiled; earlier boot/display/input acceptance only; candidate not uploaded; default env |
| `waveshare_esp32s3_lcd128` | Waveshare ESP32-S3-LCD-1.28 | paused; last build and boot log before the display code (P1.2); not built with the display code yet; if the first log lines are missing after upload, press RST |
| `native` | Host computer, unit tests only | existing SafeArea, settings, event, file, timekeeping and health suites; earlier macOS acceptance; new candidate suites not run |

| Task | Command |
|---|---|
| Build firmware | `pio run -e <env>` |
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
| Run only the event admission suite | `pio test -e native -f test_events` |
| Run only the file name suite | `pio test -e native -f test_files` |
| Run only the timekeeping suite | `pio test -e native -f test_timekeeping` |
| Run only the heap-meter suite | `pio test -e native -f test_health` |
| Set up host tools (once) | see “Code formatting” below |
| Check formatting of changed lines | `.venv-tools/bin/python scripts/check_format.py` |

Upload, erase, filesystem and monitor commands apply to firmware environments.
Native tests use Native 1.2.1 and Unity's exact `v2.7.0` Git tag, checked against
official releases on 2026-10-09. Unity's tag retains stale 2.6.0 package metadata,
so PlatformIO may display that version and download an unused registry copy;
the suite checks that the compiled header is actually 2.7.0. Other host operating
systems are not yet verified.

Firmware updates through the Web UI Update tab are implemented. The first
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
  components (LVGL, esp_lvgl_port, LittleFS, ArduinoJson and mDNS), resolved into one lockfile per chip target,
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
pool and two 9,600 B DMA draw buffers. Three fixed football snapshots consume
81,504 B; parser allocations are capped at 24 KB. Shared admission prevents
simultaneous provider TLS/JSON, image mutation and OTA. Normal JPEG redraw
can overlap provider traffic and still needs peak-memory measurement.
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) records ownership and the current
linker budget.

The stock filesystem contains two baseline JPEG aliases (7,862 logical bytes)
for the crest/slideshow and screen backgrounds. Rebuild from the high-resolution
sources when a target resolution changes:

```bash
python scripts/build_assets.py --target xiao_esp32c6_gc9a01
pio run -e xiao_esp32c6_gc9a01 -t buildfs
```

The asset/font builders require Pillow 12.3.0; ordinary firmware builds use
the committed generated fonts and need no Pillow. Font source/licence are in
`assets/fonts/`; stock artwork provenance is in `assets/src/README.md`.
The gzip Web UI is regenerated during CMake configuration.

`VERSION` is the application version source; [CHANGELOG.md](CHANGELOG.md)
describes the candidate. CMake option `FUSSBALL_RELEASE_BUILD=ON` additionally
requires a plain semantic release version and all six diagnostic/fault selectors
disabled. It does not certify runtime acceptance or publish anything.

Waveshare verification resumes after C6 acceptance. Other resolutions, controllers
and inputs still require their own buffer, font and hardware acceptance.
The C6 antenna driver is implemented; RF switching has not been tested.

## What it shows
| Screen | When |
|---|---|
| Live (single match) | Your club plays: big score, match minute, teams, highlights |
| Live (conference) | Match days: your match in the centre, others above/below |
| Table | Default outside match days: positions around your club |
| Crest / slideshow | Club crest or your own images |

The device picks the screen automatically from the situation (no match,
matchday, own club playing); which screen is shown in each situation is a
setting. With inputs (buttons or small touch modules) you can switch screens
and scroll; after a set time the device returns to the default screen.
Details: [docs/UI.md](docs/UI.md).

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
credentials and tier/coverage acceptance. Three competition routes and explicit
verified fallback fixture pairs are bounded by the shared model capacity.
Contracts and limits: [docs/DATA_PROVIDERS.md](docs/DATA_PROVIDERS.md).

## Documentation
| File | Content |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, modules, threading, memory, source layout |
| [docs/HARDWARE.md](docs/HARDWARE.md) | Boards, displays, pinouts, profile system |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Config file, `secrets.h`, settings, factory reset |
| [docs/UI.md](docs/UI.md) | Screens, navigation, round display rules, overlays |
| [docs/NETWORK.md](docs/NETWORK.md) | WiFi behaviour, setup AP, time service, REST API, OTA |
| [docs/WEB_UI.md](docs/WEB_UI.md) | Web interface tabs, image editor |
| [docs/DATA_PROVIDERS.md](docs/DATA_PROVIDERS.md) | Data APIs compared |
| [docs/DATA_MODEL.md](docs/DATA_MODEL.md) | Provider-independent data model |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Architecture decisions (ADRs) |
