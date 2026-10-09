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
ADR-017 replaces the settings part of the trial-OTA compatibility with a
simpler rule (settings kept only for an unchanged settings format).

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
**Refinement:** ADR-016 (build configuration part) replaces the manual
deletion: the build regenerates `sdkconfig.<env>` when an input changes and
verifies it against the defaults files (implemented 2026-10-09).

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

**Refinement:** ADR-016 selects a dedicated versioned OTA descriptor and the
image replacement direction.
ADR-017 supersedes the settings part of this ADR (old-readable settings during
a trial boot, migrations that support booting the previous image); the rules
for image files are unchanged.

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

**Refinement:** ADR-016 selects a live push stream for the debug transport.

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

**Refinement:** ADR-016 selects explicit verified mappings for competitions
that are actually combined.

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

**Clarification (2026-10-09):** the memory check is best effort. It allocates
and frees blocks like the port's (an upper-bound size for the port context, the
draw buffers with their size, alignment and capabilities) and reserves nothing.
If the port's context allocation still fails, `esp_lvgl_port` 2.9.0 does not
handle it safely and the device ends in a panic restart rather than the
controlled restart. "The area is redrawn" means the screen is invalidated at the
next run of the periodic supervision timer and redrawn by LVGL afterwards; there
is no fixed recovery deadline (see `docs/ARCHITECTURE.md`).

## ADR-016 — Service design directions · Accepted 2026-10-09

Refines ADR-006, ADR-008, ADR-011, ADR-012 and ADR-014. Selects the design
direction for planned services; concrete layouts, limits and values are still
designed, measured and approved before each service is implemented.

**Decisions:**
- **OTA identity:** a dedicated, versioned image descriptor carries the target
  profile and partition-layout identity and is checked before activation.
  The trial image confirms itself after a bounded local health check that
  needs no router, provider or SNTP.
- **Network operations:** all network services follow one coordinated, bounded
  operation policy (admission of competing operations, deadlines, cancellation,
  socket, body and parser limits) instead of independent per-service limits.
  Passwords stay optional; when set, they protect mutations and OTA
  consistently; a GET request never performs a mutation.
- **Relevant fixtures:** round identity plus a bounded date horizon, with
  explicit unknown/stale rules. Horizon values are set from real provider
  samples.
- **Image replacement:** a new image is written to one bounded temporary file
  inside a reserved part of the filesystem and is published only when no
  reader uses the old file; deletion follows the same reader rule.
- **Browser debug output:** a live push stream with bounded sending and
  explicit drop/disconnect behaviour; protocol and limits are chosen from
  measurements.
- **Mixed providers:** explicit, verified mappings (competition, season, team,
  fixture) with provenance and field precedence, only for competitions that
  are actually combined. Until then each provider is used on its own.
- **Reproducible configuration:** generated configuration and the component
  lockfile are regenerated and checked deterministically per target, so stale
  `sdkconfig.<env>` files or lock drift are detected instead of accepted.

**Alternatives considered:** OTA identity encoded in existing version/project
fields (field limits, mixed meaning); per-service network limits with a global
admission check (more cross-service acceptance work); a smaller fixture
selection with targeted catch-up queries (more selection logic); versioned
image file generations (more metadata); browser polling of a current debug
record (less immediate, repeated HTTP cost); provider-scoped data without
mappings (no combination); manual documented regeneration (operator burden).

**Consequences:** each direction still needs its design with measured
static RAM, heap peak, stack, flash and storage costs. The concurrency rule,
snapshot ownership, settings persistence and formatter policy are decided in
ADR-017.

## ADR-017 — Settings record, snapshot copies, serialized heavy work, formatter · Accepted 2026-10-09

Supersedes the settings part of ADR-011; refines ADR-007 and ADR-016.

**Context:** firmware updates are frequent during development and rare in
normal use. A settings store that keeps old-readable generations through every
OTA trial costs NVS space, code and test effort. Only the display shows live
scores and tables; the Web UI does not offer them to the user.

**Decisions:**
- **Settings persistence:** settings survive every restart and power loss.
  The whole settings model is one NVS blob with a header (format version,
  length, CRC32). NVS writes a changed blob completely before removing the old
  one and discards incomplete blobs at start-up (verified in the ESP-IDF 6.1.0
  source), so a save is all-or-nothing without own transaction code. Reset
  erases the whole settings namespace. Settings are **not** guaranteed across
  firmware updates: they are kept for an unchanged format version; a format
  change or a rollback to older firmware starts with `secrets.h` values and
  defaults. The version field allows a migration to be added later.
- **Snapshot ownership:** one writer publishes the football data. Readers take
  short protected copies of what they need; the lock is held only while
  copying. The display task is the reader of live data; Web handlers read only
  small status/debug data, and selection lists (competitions, teams) come from
  their own bounded requests. No reader leases on snapshot slots.
- **Concurrency:** heavy operations (TLS fetch with parsing, JPEG decoding,
  upload handling) run one at a time by default while display, input and
  status stay responsive. An overlap is allowed only where it has been measured
  and bounded (Web server request handling while the display works). The
  integration-stage reserves stay in force for the next stage: largest free
  block at least 32 KB, minimum free heap at least 40 KB, at least 25 % free in
  every task stack, application slot at most 85 % full.
- **Formatting:** clang-format in one pinned version (23.1.3, latest stable on
  2026-10-09), a project style file matching the existing code, and a
  check-only command for project sources. No mass reformatting. Host tooling
  only; no firmware cost.

**Alternatives considered:** versioned settings generations or a generation
manifest (consistent across OTA trials, more NVS/code/tests); reader leases on
several snapshot slots (no copying, but more full snapshot buffers and a
writer that can be blocked by a slow reader); permitting bounded overlaps of
heavy work in general (more peak memory); a style file without a pinned tool or
no formatter (drift not detected).

**Consequences:** after a settings format change the device must be
configured again (setup access point if `secrets.h` has no WiFi credentials).
Snapshot copy and settings record sizes, NVS use and formatter setup are
measured or verified when the modules are implemented.

**Refinement:** ADR-018 makes the settings record append-only; adding settings
is not a format change.

## ADR-018 — Append-only settings record · Accepted 2026-10-09

Refines ADR-017 (settings persistence).

**Context:** under ADR-017 every change of the settings format resets all
settings, including WiFi credentials. During development new settings are
added often, so the device would fall back to the setup access point after
many updates.

**Decision:** the settings record has a fixed, explicitly defined field layout
and is extended only at its end. For the same format version, a shorter record
is read and the missing fields take their defaults; a longer record is read
and its unknown tail is ignored. The format version increases only when a
field is reordered, removed or changes meaning or type; then ADR-017's reset to
`secrets.h` values and defaults applies.

**Alternatives considered:** a version increase for every new field (simplest
rule, settings lost on every extension); a migration routine per version (keeps
settings across all changes, more code and tests).

**Consequences:** fields are never reordered or reused; obsolete fields stay as
reserved space. Older firmware that saves after a rollback writes its shorter
record, so newer fields return to defaults afterwards. Cost estimate
(unmeasured): about 100–200 B flash for length handling, no additional RAM.
Tests cover shorter and longer records of the same version.

## ADR-019 — Event bus and logging levels · Accepted 2026-10-09

Implements the event-bus part of ADR-016 (service design directions).

**Context:** services need to learn about changes (settings, network state,
new data, OTA) without polling each other, and the UI needs a path for input
actions. RAM is the tightest resource on the ESP32-C6 (no PSRAM), and an
event system that allocates per post or grows under load would add heap
churn and unbounded latency. Log output had no common rules yet.

**Decision:**
- An own `esp_event` loop with a dedicated task (`events`, priority 5, stack
  2304 B), separate from the ESP-IDF default loop that WiFi uses.
- Events are notifications with at most 4 B payload, kept inside the queue
  entry, so no post allocates heap. State events coalesce while one is
  queued; UI actions have a bounded number of queue slots (4) and are
  dropped and counted when they are full. The queue is sized so that every
  state event always finds room. Commands that need a result are direct
  function calls, never events.
- The settings store posts "settings changed" after every save or reset;
  this replaces polling of the settings generation.
- Logging: one tag per module (device tests `<module>_test`); E = a function
  is lost or defaults apply, W = degraded but self-corrected, I = state
  changes and boot facts (no periodic output except the switchable status
  log), D/V = development only; no secrets in logs; repeated errors are
  throttled. Development builds keep INFO as default and maximum level;
  no separate log build variant now. The product log level is decided with
  the release configuration.

**Alternatives considered:** the ESP-IDF default loop (no extra task, ≈ 2.3 KB
less RAM, but a slow subscriber delays WiFi events and the queue is shared);
direct callbacks without a queue (no task, but subscribers run in the
poster's task and context); events with copied payloads (simpler receivers,
heap allocation per post and stale copies); a separate debug build variant
with verbose logging (more build configurations to maintain).

**Consequences:** receivers fetch current state themselves after a
notification. A keypress can be lost under overload; this is counted and
logged. All callbacks share the `events` task stack and must stay short.
Measured cost (`xiao_esp32c6_gc9a01`): firmware +5,568 B, of which the
`esp_event` library ≈ 3.1 KB; static RAM +116 B. Heap at boot (task stack,
queue, loop records) ≈ 3 KB estimated, device measurement pending.
