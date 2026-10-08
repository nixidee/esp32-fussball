# esp32-fussball

Football information for **one club** on small (round) ESP32 displays: live
scores, conference view of all running matches, league table and club crest.
Configured through a built-in web interface.

> **Project status: early scaffold (phase P1).** The firmware only prints a
> boot log (incl. the selected hardware profile) so far. Planned features are described in [docs/](docs/).

---

## Command reference

Run all commands in the project folder. Replace `<env>` with one of the
environments below.

| Environment (`<env>`) | Hardware | Status |
|---|---|---|
| `xiao_esp32c6_gc9a01` | Seeed XIAO ESP32-C6 + 1.28" GC9A01 round display | builds (boot log only); default env |
| `waveshare_esp32s3_lcd128` | Waveshare ESP32-S3-LCD-1.28 | builds, boot log confirmed (P1.2); if the first log lines are missing after upload, press RST |
| `native` | Host computer, unit tests only | planned |

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

Firmware updates over WiFi via the web interface (Update tab) are planned.

Build configuration files:
- `platformio.ini` — one environment per hardware target.
- `sdkconfig.defaults` (all targets) and `targets/<target>.sdkconfig.defaults`
  (board-dependent: flash size, PSRAM, console, partition file) — ESP-IDF
  settings. The generated `sdkconfig.<env>` is a build artefact: **after
  changing a defaults file, delete `sdkconfig.<env>`**, otherwise the old
  values stay in effect.
- `partitions/*.csv` — flash layout per flash size.
- `boards/`, `displays/`, `targets/<target>.h` — hardware profiles (pins,
  display, inputs); the env's `build_flags` selects the target. Wrong pin
  assignments fail the build. Details: [docs/HARDWARE.md](docs/HARDWARE.md).

---

## Quick start (planned)
1. Install [PlatformIO](https://platformio.org/install) (VS Code extension or CLI).
2. Optional: copy `include/secrets.h.example` to `include/secrets.h` and fill
   in your WiFi, club and API keys. Without it the firmware builds with
   defaults and everything is set up in the Web UI. See
   [docs/CONFIGURATION.md](docs/CONFIGURATION.md).
3. Connect the board via USB.
4. Upload firmware and filesystem:
   `pio run -e <env> -t upload` and `pio run -e <env> -t uploadfs`.
5. Without WiFi credentials the device opens the setup network
   `Fussball-XXXX`. Connect with your phone; the configuration page opens
   (or browse to `http://192.168.4.1`).
6. With WiFi the device shows its IP address for 60 s. Open it in a browser
   (or `http://fussball.local`).

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
| Seeed XIAO ESP32-C6 | 1.28" GC9A01 240×240 round (external, no backlight pin) | wiring confirmed, firmware test in P1; 3 touch modules; optional external antenna |
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
