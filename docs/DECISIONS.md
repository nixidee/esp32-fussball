# Architecture decisions (ADR)

Status values: **Proposed** (waiting for approval) · **Accepted** · **Superseded
by ADR-xxx**. Accepted ADRs change only via a new ADR.
Dated entries preserve their original decision context; ADR-022 records the
current candidate and the explicit ownership/budget refinements. Current code
and contracts are described in ARCHITECTURE and the relevant product documents.

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
transport remains planned; console status logging is controlled by stored
`debug_status_log` and `debug_status_interval_s` settings.

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
- **Time service (refinement accepted 2026-10-09):** implement per-boot
  wall-clock validity, notifications, zone application, monotonic durations
  and civil windows without networking. SNTP wiring and its network device
  test move together to P3.1. Use standard C-library time conversion with
  the firmware's existing Picolibc; only the time service sets global `TZ`.
  A compiled worldwide list of 40 location labels maps to recurring POSIX
  rules; Berlin stays the default. No new task or provisional WiFi stack.
  Host tests use macOS's library; offline device tests set the real clock
  and verify the firmware library, validity, notifications, both jump
  directions, daylight-saving changes and midnight windows.

**Alternatives considered:** OTA identity encoded in existing version/project
fields (field limits, mixed meaning); per-service network limits with a global
admission check (more cross-service acceptance work); a smaller fixture
selection with targeted catch-up queries (more selection logic); versioned
image file generations (more metadata); browser polling of a current debug
record (less immediate, repeated HTTP cost); provider-scoped data without
mappings (no combination); manual documented regeneration (operator burden).
For the time refinement: provisional WiFi/SNTP now (network integration
twice), deferring all time work (blocks pure service acceptance), a custom
POSIX-rule calculator (more maintained date arithmetic), a short European
list (less coverage), and free-form user POSIX rules (extra validation and
error-prone configuration).

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

**Time-fields extension accepted 2026-10-09:** append a location label
(maximum 32 characters) and NTP server (maximum 63, default `pool.ntp.org`)
to format 1 now; the latter is used when SNTP is connected in P3.1. Store
the zone as text, so list reordering cannot change saved values. The record
grows from 238 to 335 bytes; the C6 model from 230 to 328 bytes including
alignment. Earlier records remain valid. The optional preset becomes
`SECRET_TIME_ZONE`; the old `SECRET_TIMEZONE` name fails with a migration
message. Renaming the key preserves its value, so an earlier POSIX value
must separately be replaced with a supported location label. An invalid
preset is logged by field name and the default applies.

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
  this replaces polling of the settings generation. The time service posts
  `kTimeChanged` after applying a different zone or successfully setting
  the clock. This fifth state kind adds one queue entry (16 B), bringing
  the queue to nine entries while keeping the same task and UI slots.
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
queue, loop records) 2,808 B measured on the device before the time event
was added; the flood test passed. Current boot logs measure the extended
queue; the offline time test also verifies real subscriber delivery.

## ADR-020 — File service on LittleFS · Accepted 2026-10-09

Implements the filesystem part of ADR-011 (no automatic formatting).
Implemented and device-tested; library, mount policy, name rule and the
deferral of quota and replacement protocol confirmed by the owner.

**Context:** images live in a LittleFS partition shared by both OTA slots.
A mount can fail because storage was never used (erased flash) or because
the content is damaged. Formatting on every failed mount, the usual library
default, would silently destroy images after a fault that might be
transient or recoverable.

**Decision:**
- Library `joltwallet/littlefs` from the ESP Component Registry (the
  LittleFS port that ESP-IDF itself does not ship), pinned to an exact
  version. It was already used by the integration probe on the C6.
- Mount with `format_if_mount_failed` off. On a failed mount the whole
  partition is read: only completely erased storage is initialised; any
  other content is kept and reported unavailable until the explicit reset.
- Flat file names, 1–31 characters of `a–z 0–9 _ - .`, no leading dot (dot
  names are reserved for the service's temporary files).

**Alternatives considered:** `format_if_mount_failed` on (simplest, but
destroys damaged content without asking); never initialising automatically
(a freshly erased device would need a manual reset before first use);
SPIFFS from ESP-IDF (no directories, slower mount and weaker power-loss
behaviour than LittleFS); FAT with wear levelling (larger overhead, a
power cut during a write can damage the file table); directories and free-form names
(more validation, more attack surface for Web uploads, not needed for a few
images).

**Consequences:** flash +42 KB (library ≈ 31.8 KB), heap 1.7 KB while
mounted plus ≈ 0.9 KB per open file. A damaged filesystem leaves the device
without images until the reset is triggered. The quota, the replacement
reserve and the reader-aware publish/delete protocol (ADR-016) are still to
be implemented before images use the service: the quota and reserve with
the image storage format, the replacement protocol with the image upload.

## ADR-021 — Health diagnostics, heap ownership and browser scope · Accepted 2026-10-09

Refines ADR-012, ADR-016 and ADR-019. The design was accepted on 2026-10-09;
C6 console/device acceptance for the current core-service load passed on
2026-10-10. Complete product and browser resource acceptance remain later work.

**Context:** runtime status needs bounded all-task snapshots and reliable heap
minimum labels. ESP-IDF's local heap monitor is global: overlapping starts
reset the active interval, and its bookkeeping allocation asserts on failure
in the pinned SDK. App-loop progress must be supervised even when periodic
console output is disabled. Browser diagnostics need a defined source scope
before introducing network buffers or a console hook.

**Decision:**
- Run health reporting in the existing app task and reuse the stored console
  switch/interval. Fixed tables cover at most 16 tasks; copy names under the
  single-core scheduler pause and format afterwards. Overflow produces no
  partial/stale rows. Enable FreeRTOS trace without CPU runtime statistics
  or its text-formatting helpers.
- Register `app_loop` with the existing 5 s task watchdog after startup and
  feed it after each completed polling iteration, independently of console
  settings. Preserve IDLE/LVGL supervision and the existing abnormal-reset
  headless policy; this does not supervise event-subscriber liveness.
- Give one atomic coordinator exclusive ownership of the SDK heap interval.
  RAII meters acquire once without retry or waiting; contenders capture only
  points/timing. Read the final minimum before stop, release only after
  successful stop, and quarantine on stop failure. Failed start releases the
  gate. Observations validate lifecycle/generation and carry explicit
  lifetime/interval/unavailable scope and interval-start time. SDK region
  minima describe all concurrent allocations and are not caller attribution.
- Correct only the monitor's allocation-failure path in a project-maintained,
  build-local source copy for exact ESP-IDF 6.1.0. Verify the original source
  hash and stop the build on mismatch; leave the shared SDK untouched. Return
  `ESP_ERR_NO_MEM` before resetting minima; allocation aborts stay disabled.
- Browser output is controlled application status/state/error records,
  live only, with secrets omitted/redacted before enqueueing. No retained
  history or arbitrary SDK/vendor console mirroring. Producers never block;
  bounded drop/disconnect behaviour is explicit. Transport is deferred to
  P8.7; protocol and inactive/active/client/message/rate budgets require
  measurement before implementation. No hook or network stack is added now.

**Alternatives considered:** retaining the SDK allocation panic (a diagnostic
can restart an otherwise running device); deferring local intervals until a
later SDK (no phase minimum now); mirroring the entire console to browsers
(uncontrolled source/formatting and secret-exposure scope); a dedicated health
task (another task stack and supervision context).

**Consequences:** no new settings layout, partition or normal-runtime task.
The C6 task tables occupy 960 B; trace also expands task, queue/semaphore,
event-group, timer and stream/message-buffer control objects, as listed in
[ARCHITECTURE.md](ARCHITECTURE.md#health-service-implemented-c6-console-acceptance-passed).
Phase meters add caller-stack storage and the SDK's bounded interval
allocation. SDK upgrades require review of the guarded correction. The fix
covers this monitor allocation only, not general firmware OOM recovery.
Extended C6 console checks passed, including a 70 s closed serial client
observed without changing reset control lines, and normal firmware was restored.
This covers the current boot/core-service workload; the complete resource
budget and browser transport have their own later acceptance.

## ADR-022 — Complete bounded candidate under delegated decisions · Accepted 2026-10-10

**Context:** the owner explicitly requested autonomous continuation until the
scheduled implementation is complete except polish and testing. The owner is
unavailable for questions. Existing features, C6-first scope and earlier
storage/identity choices still apply; no commit, push, release or device upload
was requested.

**Decision:**
- Complete network/data/UI/images/Web/OTA inside the existing component
  boundaries. Use the existing app/LVGL tasks and their lock for presentation,
  one provider worker and a static debug worker. Reuse one fixed scene rather
  than creating/destroying four object trees.
- Keep the existing dual-OTA/LittleFS/NVS layout. The initial format-1 append
  choice is superseded by ADR-023's format-2 encoding and legacy import,
  including three competition/fallback routes and eight explicit fixture pairs.
  Model/record are 1,736/1,720 B on C6. Provider identity stays scoped;
  names never establish a cross-source fixture match.
- Keep three fixed 27,168 B snapshots, one 24 KB capped streaming parser and
  a 30-second operation deadline with one-second socket waits. Async DNS uses
  one fixed record in the existing lwIP thread; incremental TLS preserves
  hostname verification/SNI. No task closes another task's TLS context.
- Serialize provider TLS/JSON, image mutation and OTA. Normal LVGL JPEG redraw
  is permitted during provider work inside its fixed pool, but its full-load
  peak is not accepted by delegation.
- Use exact-resolution baseline JPEG with complete decoder validation,
  32 KB per file, 320 KB unique image quota and 40 KB replacement reserve.
  Ten logical slots share two stock aliases by default. Publish/delete under
  the LVGL lock after invalidating the old image cache reader.
- Use the full IDF CA bundle, client TLS, IPv4 and ordinary WiFi authentication.
  Disable unused C6 SDK IPv6/enterprise/IRAM speed options to reduce memory;
  throughput/RF acceptance is pending. Retain all public planned C6 features.
- Pin ArduinoJson 7.4.3 and mDNS 1.14.0 after primary registry checks on
  2026-10-10. Keep the previously accepted platform/IDF/LVGL/port/LittleFS
  pins. Pillow 12.3.0 is a host asset-generation tool, not device code.
- Bound browser debug to one client, one 256-byte in-flight record, once per
  second, with a static 4 KB producer stack and explicit drop/disconnect.
  Mutations use optional password plus nonce/session and origin checks;
  reads redact keys and credentials.
- Version firmware identity independently from app version. Check a 92-byte
  target/layout/settings descriptor before OTA writes, validate the final
  image and confirm only after ten seconds of local health. Trial storage and
  durable budget mutations are blocked; router/provider success is not needed.
- Use `VERSION`, development suffixes and an opt-in release source guard.
  A source/build guard never stands in for runtime or release acceptance.

**Alternatives and costs:** parallel heavy work needs additional simultaneous
TLS/parser/upload memory and sockets. Raw full-screen RGB565 costs 115,200 B
per 240×240 image and cannot meet the five-image goal in this filesystem.
A separate UI task adds stack and another handoff protocol. Automatic
cross-provider name matching weakens identity. Larger partitions consume image
space and change OTA compatibility. These alternatives are not adopted.

**Measured consequence before ADR-023:** C6 binary 1,607,344 B, PlatformIO flash 1,606,596 B
(87.6% of the app slot), static RAM 196,736 B and linked DIRAM 266,066 B.
This is +1,091,040 B binary / +148,516 B static RAM over the accepted core image.
Stock files occupy 7,862 logical bytes. The app fits but misses the 85% flash
reserve target by about 46.8 KB. Heap, largest block, stacks, LVGL and filesystem
metadata/replacement peaks remain unmeasured for the complete candidate.

**Acceptance boundary:** implementation and C6/filesystem build only.
Provider fixtures/real keys, browser/device, power loss/rollback, maximum load,
new resolutions, S3 and long-run tests are pending. Visual/resource polish,
screenshots and the already deferred enhancement backlog remain. No reserve,
coverage or production acceptance is implied.

## ADR-023 — Settings size ceiling, format 2 and legacy import · Accepted 2026-10-10

**Context:** final source review proved that the accepted core firmware refuses
stored records above 1024 bytes, while the complete candidate encodes 1720.
Keeping version 1 would incorrectly promise old-reader compatibility. The
owner's autonomous implementation instruction authorizes a documented correction.
This supersedes ADR-022's initial format-1 choice and clarifies ADR-018's append
rule at the reader-size boundary; neither earlier decision proved an unlimited
forward-readable record.

**Decision:** encode format 2 at the same payload offsets and total size.
Accept valid format-1 records only within their old 1024-byte bound; import the
known core through byte 334, ignoring unknown legacy tail after length/CRC
validation. Missing core fields retain initial values; partial/invalid fields
fail atomically. New groups retain initial defaults/presets. Loading does not
rewrite storage. The next explicit save writes format 2, and trial writes
remain blocked. No reverse or arbitrary future-format migration is added.

The OTA identity declares settings format 2 and upload now explicitly compares
that field with the supported current identity before writing. Future schema
transitions need their own declared policy; the current browser gate accepts
matching format 2 only. An automatic trial rollback sees the untouched legacy
record. A manual USB downgrade after an accepted format-2 save uses old-firmware
initial values. It does not recover the new configuration automatically.

**Alternatives:** keep format 1 and disclose silent loss on old firmware
(contradicts the compatibility claim); constrain/drop routes/mappings to fit
1024 bytes (reduces features); invent compressed fields without a schema change
(changes the existing encoding contract); reset every legacy record immediately
(loses valid WiFi/time settings). A bounded read-only import preserves those
settings without a second NVS record or extra write protocol.

**Cost and boundary:** model/record stay 1736/1720 B, with no extra static RAM,
heap, filesystem or NVS quota. A span/version/branch uses bounded local scalars.
The C6 build adds 64 B flash and zero static/DIRAM growth. Import, explicit save,
CRC/partial-tail cases and actual rollback/downgrade tests are pending.
The format-2 correction artifact, before subsequent network fixes, was
1,607,408 B binary, PlatformIO flash 1,606,660 B (87.6% of the
1,835,008 B app slot), static RAM 196,736 B and DIRAM 266,066 B. The 85%
flash reserve goal is missed by 46,904 B. No device/storage write or behavioural
test was performed for this correction.
