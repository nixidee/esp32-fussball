# Architecture decisions (ADR)

Status values: **Proposed** (waiting for approval) · **Accepted** · **Superseded
by ADR-xxx**. Accepted ADRs change only via a new ADR.

---

## ADR-001 — Framework: ESP-IDF (no Arduino) · Accepted 2026-10-08
**Context:** C6 without PSRAM (512 KB SRAM), 4 MB flash with dual OTA;
needs LVGL, HTTPS, web server with uploads, OTA with rollback, detailed WiFi
disconnect handling. No Arduino-only libraries required. Official PlatformIO
platform supports the XIAO C6 only with `espidf`; Arduino needs the pioarduino
fork. An earlier project of the author already runs ESP-IDF + LVGL +
esp_lcd on an ESP32-S3 with the same display.
**Decision:** pure ESP-IDF, C++.
**Alternatives:** Arduino via pioarduino (simpler API, more RAM/flash, less
control); mixed Arduino+IDF (largest binary, known config pitfalls).
**Consequences:** more boilerplate; best control of memory and WiFi; no
Arduino libraries.

## ADR-002 — Platform package and versions · Accepted 2026-10-08
**Decision:** official `platformio/espressif32` **7.1.3** (newest registry
version, 2026-09-11; bundles ESP-IDF 6.1.0 = newest IDF minor release; checked
2026-10-08). Re-check for newer releases before every upgrade.
**Consequences:** all component versions pinned in `idf_component.yml`.

## ADR-003 — Display stack: esp_lcd + LVGL 9 · Accepted 2026-10-08
Versions (latest stable, 2026-10-08): `lvgl/lvgl` 9.6.0~1,
`espressif/esp_lvgl_port` 2.9.0, pinned in `components/display/idf_component.yml`.
**Context:** screens composite a background image, text and a semi-transparent
overlay inside a round area; a full 240×240 RGB565 frame buffer costs 115 KB
(22 % of C6 SRAM). LVGL renders in partial bands and redraws only changed
areas. Reference cost: LVGL core ≈ 250 KB flash in an earlier ESP32-S3
build; the LVGL 9.5 docs recommend > 180 KB flash and > 48 KB heap.
**Decision:** `esp_lcd` SPI panel IO with own GC9A01 init sequence
(based on an earlier hardware-verified driver, re-implemented cleanly), LVGL 9.x with a trimmed
configuration for the C6 (unused widgets/features off, small `LV_MEM_SIZE`,
partial double draw buffers), glued via `espressif/esp_lvgl_port`. Fit is
verified by measurement on the C6 with WiFi and TLS; if RAM is
insufficient, the XIAO ESP32-S3 is the fallback board.
**Alternatives:** LovyanGFX (C6 not listed as supported in its README, 2026-10-08); own band
renderer (re-implements compositing, fonts, JPEG).
**Consequences:** display drivers are swappable per display profile.

## ADR-004 — Hardware profiles: board + display + target · Accepted 2026-10-08
**Decision:** compile-time profiles in `boards/`, `displays/`,
`targets/`; one PlatformIO env per target; static checks for pin conflicts.
**Alternatives:** one header per env (duplication between targets sharing a
board/display); runtime JSON config (cannot select drivers/flash layout).
**Consequences:** new board or display = new header + env.

## ADR-005 — Data providers behind a canonical model · Accepted 2026-10-08
**Decision:** provider modules map to a canonical model with
capability flags; OpenLigaDB default; API-Football and ESPN optional; routing
per competition (P9). Club selected per provider for the start; the
structure allows adding a canonical club mapping later with small changes.
**Consequences:** presenters/views never depend on a provider.

## ADR-006 — Web UI embedded in firmware · Accepted 2026-10-08
**Context:** separate filesystem uploads can leave UI and firmware out of sync.
**Decision:** Web UI sources in `web/`, minified + gzipped at build
time and embedded in the firmware; LittleFS holds images. Own images are
uploaded via the Web UI; default images are updated only over USB
(`uploadfs`) (decided 2026-10-08).
**Alternatives:** UI on LittleFS (smaller app, needs `uploadfs` and can
mismatch).
**Consequences:** larger app image (matters on the C6; to be measured); one
OTA updates firmware and UI together.

## ADR-007 — Settings: single config header + device persistence · Accepted 2026-10-08
**Decision:** `include/app_config.h` holds every default, limit and
enum plus schema version; device exposes the schema to the Web UI.
Persistence options:
- A: **NVS** — wear levelling, atomic per key, survives LittleFS format;
  per-key size limits; less human-readable.
- B: **LittleFS JSON file** — readable, easy export/import; needs atomic write
  (temp + rename) and lives in the filesystem quota.
Chosen: A (NVS) for settings, JSON only for export/import.
Requirements (2026-10-08):
- A reset to defaults must always be possible (Web UI and device), and
  no stale value may survive it or a firmware/schema change.
- `include/secrets.h` is **optional**: the build never fails without it.
  Missing values fall back to the defaults in `app_config.h` (no WiFi →
  setup AP; no club/league → default image).

## ADR-008 — Image pipeline: browser composes, device stores · Accepted 2026-10-08
**Decision:** the browser renders the final image at device
resolution (crop, move, transparency pre-blended, round mask preview) and
uploads it in the device format. Images must be uploadable; the browser
downscales cleanly to device resolution; upload size is limited.
Format (2026-10-08, conditional on LVGL): baseline JPEG decoded by
LVGL's built-in TJPGD (tile-wise, low RAM). Compressed RGB565 (RLE/LZ4) is
decoded completely into RAM by LVGL (LVGL 9 docs) → not used for full-screen
images on boards without PSRAM. Decided 2026-10-08: images at display size;
uncompressed RGB565 or baseline JPEG are both acceptable. Default images
generated per resolution by a build script from high-res sources.
**Consequences:** no heavy image processing on the device; per-resolution
asset sets.

## ADR-009 — JSON parsing: streamed with filter · Accepted 2026-10-08
**Context:** provider responses are 7–14 KB JSON; the parse runs while TLS
holds its peak heap. A DOM parser (cJSON) keeps the whole document plus a
node tree in RAM.
**Decision:** `bblanchon/arduinojson` (plain C++, no Arduino dependency)
reads directly from the HTTP stream with a filter, so only the fields the
canonical model needs are stored. Provider mappers own their filters.
**Alternatives:** `MaJerle/lwjson` stream parser (true token callbacks,
smallest RAM, not in the ESP registry, more mapping code) — fallback if
measurements show ArduinoJson too large (needs new approval); own parser; cJSON/jsmn
(whole document in RAM).
**Consequences:** response buffer never held completely; filtered-document
size per endpoint measured and bounded.

## ADR-010 — Board-dependent ESP-IDF settings per target · Accepted 2026-10-08
Extends ADR-004 (decided 2026-10-08).
**Context:** ESP-IDF needs some board facts as Kconfig values (flash size,
PSRAM mode, console channel, partition table file). They cannot come from a
C header. A file per chip (`sdkconfig.defaults.<chip>`) breaks as soon as two
boards share a chip (e.g. Waveshare S3: 16 MB, quad PSRAM vs. XIAO S3: 8 MB,
octal PSRAM).
**Decision:** `targets/<target>.sdkconfig.defaults` next to
`targets/<target>.h`. Each env lists `sdkconfig.defaults;targets/<target>.sdkconfig.defaults`
via `board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS=...`. No
`sdkconfig.defaults.<chip>` files.
**Alternatives:** per chip (breaks with a second board per chip); per-target
file in the project root (target facts spread over two places).
**Consequences:** every target fact lives in `targets/`; a new target = header
+ sdkconfig file + env. PlatformIO re-runs CMake only when
`sdkconfig.defaults` changes → after editing a target file delete
`sdkconfig.<env>`. Verified 2026-10-08: generated sdkconfig of both envs
identical before and after the move.
