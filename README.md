# esp32-fussball

Football information for **one club** on small (round) ESP32 displays: live
scores, conference view of all running matches, league table and club crest.
Configured through a built-in web interface.

> **Project status: early scaffold (phase P1).** The XIAO ESP32-C6 target
> shows an LVGL boot test screen and logs touch input changes; the Waveshare
> target was last accepted for boot logging before display integration and is
> paused until the C6 is done. Pure SafeArea geometry has native host tests;
> display failure handling is implemented and was verified on the device. A
> temporary diagnostic build (since removed, kept in the Git history) showed that
> WiFi, HTTPS to OpenLigaDB, JSON parsing, a JPEG background and a small HTTP
> server fit the ESP32-C6 with reserves; this is integration evidence, not a
> certification of the finished application. The build regenerates and
> verifies its ESP-IDF configuration; core services come next.
> Planned features are described
> in [docs/](docs/); a successful boot screen does not certify the complete C6 app.

---

## Command reference

Run all commands in the project folder. Replace `<env>` with one of the
environments below.

| Environment (`<env>`) | Hardware | Status |
|---|---|---|
| `xiao_esp32c6_gc9a01` | Seeed XIAO ESP32-C6 + 1.28" GC9A01 round display | boot test screen (LVGL) and touch input log confirmed on the device; boot log lines are not visible over USB (input logs are); default env |
| `waveshare_esp32s3_lcd128` | Waveshare ESP32-S3-LCD-1.28 | paused; last build and boot log before the display code (P1.2); not built with the display code yet; if the first log lines are missing after upload, press RST |
| `native` | Host computer, unit tests only | SafeArea geometry suite; C++20, verified on macOS with Apple clang 21.0.0 |

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
| Set up host tools (once) | see “Code formatting” below |
| Check formatting of changed lines | `.venv-tools/bin/python scripts/check_format.py` |

Upload, erase, filesystem and monitor commands apply to firmware environments.
Native tests use Native 1.2.1 and Unity's exact `v2.7.0` Git tag, checked against
official releases on 2026-10-09. Unity's tag retains stale 2.6.0 package metadata,
so PlatformIO may display that version and download an unused registry copy;
the suite checks that the compiled header is actually 2.7.0. Other host operating
systems are not yet verified.

Firmware updates over WiFi via the web interface (Update tab) are planned.
Routine firmware OTA will preserve images in LittleFS. USB `uploadfs` is for
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
  components (LVGL, esp_lvgl_port), resolved into one lockfile per chip target,
  `dependencies.lock.<chip>` (e.g. `dependencies.lock.esp32c6`), set in the
  root `CMakeLists.txt`. Lockfiles are tracked. The component manager changes
  a lockfile only when a manifest changes; review and commit both together.
  The first build of a new chip target creates its lockfile.
- `scripts/native_sources.py` — explicit pure-component selection for host
  tests; firmware and tests compile the same geometry sources without copies.
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

## Quick start (planned)
1. Install [PlatformIO](https://platformio.org/install) (VS Code extension or CLI).
2. Optional: copy `include/secrets.h.example` to `include/secrets.h` and fill
   in your WiFi, club and API keys. Without it the firmware builds with
   defaults and everything is set up in the Web UI. See
   [docs/CONFIGURATION.md](docs/CONFIGURATION.md).
3. Connect the board via USB.
4. For initial setup/development, upload firmware and filesystem:
   `pio run -e <env> -t upload` and `pio run -e <env> -t uploadfs`.
   The filesystem step replaces its contents, including uploaded user images.
   Routine updates will use firmware OTA without a filesystem upload.
5. Without WiFi credentials the device opens the setup network
   `Fussball-XXXX`. Connect with your phone; the configuration page opens
   (or browse to `http://192.168.4.1`).
6. With WiFi the device shows its IP address for 60 s. Open it in a browser
   (or `http://fussball.local`).

## Development milestones

The active target is the XIAO ESP32-C6. Host geometry tests, the bounded
display fault-path repair and the board-pin guards are done. A temporary
diagnostic build has measured WiFi/TLS, filtered JSON, JPEG rendering and a
small HTTP server together on the C6: they fit the current partition layout
with reserves, and loading OpenLigaDB data over HTTPS works. Further checks
cover the complete data model, images/Web/debug and the supported maximum load
before release. Concrete limits and resource reserves are decided before each
dependent implementation.

Waveshare verification resumes after the C6 work. The external-antenna driver
is deferred until before the WiFi manager; current firmware does not control it.

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
| Seeed XIAO ESP32-C6 | 1.28" GC9A01 240×240 round (external, no backlight pin) | display and touch confirmed on the device (P1.4); 3 touch modules; optional external antenna |
| Waveshare ESP32-S3-LCD-1.28 | built-in 240×240 round | 16 MB flash, 2 MB PSRAM; no inputs (BOOT button not reachable in the housing) |
| Seeed XIAO ESP32-S3 | 1.28" GC9A01 240×240 round (external) | later |

## Data sources
Default: **OpenLigaDB** (free, no key; Bundesliga down to Regionalliga Nord,
Nordost, Bayern; DFB-Pokal). Optional providers with API key are planned.
Comparison and limits: [docs/DATA_PROVIDERS.md](docs/DATA_PROVIDERS.md).

## Documentation
| File | Content |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, modules, threading, memory, source layout |
| [docs/HARDWARE.md](docs/HARDWARE.md) | Boards, displays, pinouts, profile system |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Config file, `secrets.h`, settings, factory reset |
| [docs/UI.md](docs/UI.md) | Screens, navigation, round display rules, overlays |
| [docs/NETWORK.md](docs/NETWORK.md) | WiFi behaviour, setup AP, REST API, OTA |
| [docs/WEB_UI.md](docs/WEB_UI.md) | Web interface tabs, image editor |
| [docs/DATA_PROVIDERS.md](docs/DATA_PROVIDERS.md) | Data APIs compared |
| [docs/DATA_MODEL.md](docs/DATA_MODEL.md) | Provider-independent data model |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Architecture decisions (ADRs) |
