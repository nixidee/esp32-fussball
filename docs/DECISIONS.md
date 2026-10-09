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
**Refinement:** ADR-015 (superseding ADR-013) adds the failure-lifetime and
recovery contract; the existing port remains part of the display stack, unpatched.

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
**Refinement:** ADR-014 supersedes the assurance that canonical club mapping
can be added with small changes. Cross-provider identity is a design gate,
not an assumed low-cost extension.

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
**Refinement:** ADR-011 defines the shared-filesystem and development USB
replacement policy; ADR-012 bounds the planned debug transport.

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

**Refinement:** ADR-011 adds whole-model consistency and trial-OTA storage
compatibility. These are requirements for the planned settings service.

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
Measured 2026-10-09 on the ESP32-C6 (see ARCHITECTURE, staged resource
acceptance): LVGL TJPGD works with a build-level symbol rename, decodes a
240×240 baseline JPEG in ≈105 ms per pass and repeats the pass for every draw
stripe (full redraw 1.1–1.25 s); raw RGB565 avoids that cost at 115,200 B per
image. The format choice stays open between these two.
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

## ADR-011 — Shared storage, consistent settings and local OTA acceptance · Accepted 2026-10-08

Extends ADR-006 and ADR-007. These service contracts are planned, not
implemented by this documentation change.

**Context:** both OTA app slots share NVS and LittleFS. Rolling back firmware
does not revert settings or image files. NVS persistence for individual keys
does not by itself guarantee one consistent complete settings model. USB
`uploadfs` replaces a filesystem image rather than merging individual files.

**Decision:** NVS remains the settings store. Saves publish a complete,
validated settings generation; interruption must yield either the previous
complete model or the new complete model. Reset clears all persisted settings,
including obsolete migration state, and optionally deletes user images only
when explicitly selected. A filesystem mount fault never triggers automatic
formatting.

Routine firmware OTA preserves NVS and LittleFS images. During a trial boot,
settings and files readable by the previous firmware remain available until
the new image is accepted. Migration, commit, retention and cleanup mechanisms
must be decided before the affected implementation.

The OTA candidate carries explicit target/profile and partition-layout
compatibility metadata, because `esp_app_desc_t` has no target-profile field.
A dedicated custom descriptor is proposed; its representation remains a design
decision. Acceptance follows a bounded local boot health check: router, provider
and SNTP availability are not prerequisites. Define the descriptor, local
checks, deadline and explicit invalidation/reset path before implementing OTA,
including recovery when no previous bootable image exists.

USB `uploadfs` is development-only and may replace user images. No
backup/restore tooling or separate image storage is added for that operation.
User image updates during normal use remain file-level Web UI uploads.

**Consequences:** transaction staging, schema compatibility, custom metadata,
validation and temporary image replacement space have resource costs that
must be measured. The design must pass power-loss, reset, failed trial and
older-firmware boot tests. No concrete buffer/quota/deadline values are
selected here. Product contracts are detailed in [CONFIGURATION.md](CONFIGURATION.md),
[NETWORK.md](NETWORK.md) and [WEB_UI.md](WEB_UI.md).

## ADR-012 — Bounded allocations and debug transport · Accepted 2026-10-08

Clarifies the resource discipline for ADR-003, ADR-006 and ADR-009. The debug
transport remains planned; only compile-time console status logging exists.

**Context:** TLS, parsing, rendering and socket libraries may allocate memory
during operations. A browser stream needs connection state and temporary
formatting buffers even without retaining logs.

**Decision:** bounded, measured library allocations are permitted within the
approved peak budget. Avoidable allocation churn and unbounded growth are
not permitted. The browser debug stream retains no log history on the device;
inactive and connected transport overhead are bounded and measured. Select
the protocol, client and message limits, and slow-client drop/disconnect
behaviour before implementation. Streaming must not block firmware tasks;
debug output remains switchable and secrets are redacted.

**Consequences:** budgets account for static memory, peak heap, stack,
fragmentation/largest blocks, flash and relevant storage quotas under allowed
concurrent workloads. "No log history" does not mean "zero connection RAM".
Concrete limits are established by the affected module's design and tests.

## ADR-013 — Reproducibly maintained display-port patch · Superseded by ADR-015

Accepted 2026-10-08, superseded 2026-10-09 before any patch was implemented.
Extends ADR-003.

**Context:** asynchronous display transfers keep DMA (direct memory access)
readers alive after submission. Errors, partial submission and teardown need
an explicit transfer-lifetime contract so a buffer cannot be reused or freed
while the driver may still read it.

**Decision:** retain `esp_lvgl_port` and apply a bounded, reproducibly
maintained patch for the required failure paths. Patch artifacts live outside
generated `managed_components`; regeneration must not silently remove them.
Decide exact patch placement, application/verification mechanics and recovery
before implementation. Define transfer ownership, completion/error propagation,
quiescence before cleanup and safe reinitialization; verify these before the
first network/image integration budget check.

**Consequences:** the patch becomes maintained project material, with an
upstream compatibility check whenever the component changes. Fault tests cover
failed/partial transfer submission, missing or delayed completion, relevant
allocation failures, cleanup and retry. Bookkeeping and handling costs are
assessed before coding and compared in the required build; this decision does
not introduce a replacement library or a new version.

## ADR-014 — Provider identity before cross-source routing · Accepted 2026-10-08

Refines ADR-005 and supersedes its statement that canonical club mapping can
later be added with small changes. Provider-scoped initial club selection and
the canonical data model remain accepted.

**Context:** providers use different identifiers and may disagree on names,
competition structure or match/event corrections. Similar names alone cannot
establish that two records describe the same entity.

**Decision:** before mixing sources, design the identity mappings for teams,
competitions, seasons and matches, data provenance, conflict precedence and
their storage/lookup costs. Name similarity may suggest a candidate for
selection but never silently merges identities. Event enrichment must use the
same explicit identity and correction contracts.

**Consequences:** cross-source routing is gated on the mapping design and its
capacity tests. No mapping representation, conflict algorithm or concrete
capacity is selected by this decision. Views continue to receive only the
canonical model.

## ADR-015 — Display failure handling without port patch · Accepted 2026-10-09

Supersedes ADR-013. Extends ADR-003. Design details and fault verification are
pending; this records the direction, not a completed fix.

**Context:** the pinned `esp_lvgl_port` does not complete LVGL's flush on a
failed draw and leaks its draw buffers when display creation fails. ESP-IDF SPI
panel IO waits without timeout for bus acquisition and transfer completion, so
a hang cannot be bounded from inside a callback. A port patch would only be
required to reinitialize the display locally without restarting.

**Decision:** keep the managed port unchanged. Hybrid recovery: known errors
with no transfer in flight are handled locally; hangs and unclear states end in
a task-watchdog panic restart, bounded against boot loops. The own GC9A01 driver
handles failed draws: a following command waits until earlier transfers have
finished, then LVGL is told the flush is complete, the error is counted and the
area is redrawn. Before adding the display to the port, the firmware checks
that enough DMA-capable memory is free; if the port still fails, the device
restarts in a controlled way. LVGL pool exhaustion is caught by its allocation
assertion and also leads to a controlled restart. Exact deadlines, the
repeated-failure bound and the required ESP-IDF/LVGL settings are decided in the
display failure design before implementation.

**Alternatives:** maintained port patch with local reinitialization (more
maintenance; still needs bounded SPI waits); own LVGL glue replacing the port
(full control; larger change of ADR-003).

**Consequences:** no third-party source is modified or mirrored. Port-internal
allocation failures are not retried locally; leaked memory is reclaimed by the
restart. Task-watchdog panic applies to every watched task, not only the
display. Costs (driver state, watchdog entry, assertion code) are measured in
the implementing build. An upstream bug report for the port is recommended.
