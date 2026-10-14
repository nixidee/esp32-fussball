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
[NETWORK.md](NETWORK.md), [API.md](API.md) and [WEB_UI.md](WEB_UI.md).

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
  (Historical: since 2026-10-14 the one writer is the provider worker and
  the published data is the compact match store, a single static object
  written under a mutex; the presenter builds its view model while holding
  that mutex. The snapshot copies and their code are deleted.)
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
- Register `app_loop` with the existing task watchdog (5 s when decided, now
  15 s as an interim value, see docs/ARCHITECTURE.md) after startup and
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
  a 30-second operation deadline with one-second socket waits. (Historical:
  since 2026-10-14 the compact match store replaces the three snapshots;
  the parser, deadline and socket waits are unchanged.) Async DNS uses
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
  2026-10-10. The host test environment uses the same ArduinoJson version
  (PlatformIO registry checked 2026-10-13) so the provider mappers, declared
  in their own header without ESP-IDF types, are tested with recorded
  responses; the firmware is unaffected. (Historical: the provider mappers
  were deleted on 2026-10-14; the same host version now tests the store
  mappers and request plans.) Keep the previously accepted platform/IDF/LVGL/port/LittleFS
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

## ADR-023 — Settings size ceiling, format 2 and legacy import · Accepted 2026-10-10 · format-2 encoding and OTA gate superseded by ADR-024

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

## ADR-024 — Settings format 3: lists, image library, equal-or-newer OTA gate · Accepted 2026-10-12 · record extended by ADR-026 · table competition replaced by ADR-033

**Context:** the fixed slots of format 2 (three routes, three fallbacks, eight
mappings, each with an enabled flag) show empty entries in the interface, cap
the record near the NVS ceiling, and tie each of ten fixed image roles to its
own file, so one upload could not be reused and "reset" was not a real delete.
The owner decided on 2026-10-11 to use real lists and an image library.

**Decision:**
- Format 3 stores ordered lists with a count and only their entries:
  competitions (max 3; each a primary source, an optional fallback source of
  another provider, active flags and stored display names), fixture pairs
  (max 8, referring to a competition by position), slides (max 5) and a crest
  image. All maxima are constants in `app_config.h`. Order has meaning: the
  first active competition supplies the league table; slides play in order.
- The record is variable length with a longest case of 1931 bytes (pinned by a
  host test) below the 2048-byte reader ceiling. The decoder converts formats 1
  and 2 in RAM (complete routes become competitions, empty slots disappear,
  mappings are renumbered); loading never writes NVS, the next save writes
  format 3 (ADR-023 behaviour kept).
- Images are a library of up to ten files `img<id>.jpg` (id 1–10, lowest free
  id on upload, real delete); id 0 is the stock image. A reference may name a
  missing image (deleted, or settings imported from another device): the
  renderer skips it, and the application removes dead references outside an
  OTA trial. Quota, validation and rename-under-display-lock are unchanged.
  The earlier fixed-name files are not migrated; they are removed once outside
  the trial and the owner uploads again.
- The browser OTA gate accepts an image whose settings format is equal or
  newer and refuses an older one. This replaces ADR-023's equality gate; the
  first format-3 image is installed by USB because the running gate still
  demands equality. Downgrade by USB after a format-3 save keeps ADR-023
  behaviour (the old firmware starts with initial values).
- The crest screen has no background image (its `image` stays 0).

**Alternatives:** keep fixed slots and only hide empty ones (the cap and the
enabled flags stay; no reuse of images); role-bound image files (no reuse, quota
spent twice); keep the equality OTA gate (every format change would need USB).

**Cost and boundary:** Model 1944 B (+208 B, of which 198 B are the stored
display names), longest record 1931 B (+211 B transient on save, heap), each
`settings::current()` copy 1.9 KB on the caller's stack. The C6 clean build is
+620 B static RAM and +3,706 B flash against the previous step; LittleFS holds
at most ten library files within the unchanged quota. The migration of a real
format-2 record, removal of the legacy image files and the first USB install
are verified on the host only; device acceptance is pending. The page in
`web/index.html` still edits the removed fixed slots until its rewrite; settings
remain changeable through export/import and the API meanwhile.

## ADR-025 — REST API for the new design: sessions, field-level errors, list and image endpoints · Accepted 2026-10-12

**Context:** the Web UI review found that a second browser logged the first
one out (one session slot), that a rejected request could end in a connection
reset instead of a readable message, that a 400 did not say which value was
wrong, and that the new list model had no endpoints for fixtures or image
files. The owner decided the open points on 2026-10-12 (OQ-89 to OQ-93).

**Decision:**
- **Sessions:** a table of four slots (`cfg::kSessionSlots`) with a sliding
  15-minute expiry; the least recently used slot is replaced by a new login.
  Saving settings keeps every session; only a changed admin password ends the
  other sessions. Wire format unchanged (`X-CSRF-Token`, `POST /session →
  {token, expires_ms}`), plus `GET /session → {password_set, valid}`.
- **Errors:** `{"error","code"[,"field"]}`. `code` is the settings reason name
  for value errors, otherwise a stable word per failure; `field` names the
  rejected value. The unread request body is drained (up to 32 KB) before the
  answer, so the browser reads it and the connection stays usable. Message
  strings the current page matches on are unchanged.
- **Validation:** settings are validated against the display profile's limits
  (`visible_matches_max`, `table_window_max`: 240 px round 4 and 7); the schema
  reports them under `bounds`, the API rejects above them, stored values are
  clamped when loaded (OQ-92).
- **Host and origin:** the Host header must name the device (case-insensitive,
  `:80` accepted), an `Origin` must be `http://` plus that host.
- **Endpoints:** `GET /fixtures` (primary from the published data, fallback
  from its own provider job started by `POST /data/fixtures`, OQ-90),
  `GET /images/file` with per-image revision and long cache lifetime (W41),
  `POST /display` for `next` and `ip` through the event bus (OQ-91), `/status`
  extended by the shown screen, the club's own match and per-competition state.
- **Debug stream:** the token stays in the URL query (a browser WebSocket
  cannot set headers); the log never prints the query; the stream ends with
  the session that opened it (OQ-93). ESP-IDF's httpd answers the WebSocket
  handshake itself and never calls the URI handler for it, so origin, Browser
  debug, token and the free slot are checked in a pre-handshake callback
  (`CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`) and a refusal is an ordinary
  HTTP error before any upgrade (verified failure on the first firmware, fix
  pending device re-check).
- **Memory evidence:** `GET /status` reports `heap_min` and the minimum free
  stack of every task, so load tests need no serial port (0 B static RAM,
  about 720 B heap scratch and 384 B HTTP task stack per call).
- One handler per route (26 of 40 registered), instead of dispatching by
  string comparison, which keeps each handler's stack frame small.

**Alternatives:** keep one session slot (fails the two-browser requirement);
a cookie session (needs a different CSRF scheme and a page change); close the
connection after every error (the previous behaviour; unverified whether it alone caused the reported resets); one
fixtures job shared with the competition lists (the picker would have to wait
for and overwrite the list the user is editing).

**Cost and boundary:** C6 clean build against L41: firmware.bin +18,448 B,
PIO flash +18,444 B (82.8 %, 39,858 B below the 85 % line), static RAM +312 B,
gzip asset unchanged. HTTP handlers 26 of 40. Bodies above 32 KB (firmware,
oversize images) cannot be drained, so their rejection can still end in a reset
connection; the page must check sizes before uploading. `web/index.html` is not
adapted at that earlier checkpoint: its removed fixed-slot accesses actually
aborted startup before a session could be created; the earlier claim that
other sections kept working was incorrect. Device checks on 2026-10-12: the W03 reproduction, two sessions
saving in parallel for 16.5 minutes, field names on every value error and
keep-alive after errors pass; the debug socket, image replace/delete and the
heap and stack reserves are checked after the pre-handshake fix is flashed.
Browser behaviour is not verified.

### Browser rewrite completion and additive API fields · 2026-10-12

The page now implements six task-oriented sections with an independent startup,
one global draft/save, schema-based validation, explicit secret modes and
saved-only fixture picking. Search/scan/error states are observable rather
than inferred from English error strings or fixed delays. The endpoint adds
scan completion/error, full ELF build identity, actual OTA capacity and a
separate confirmed image-storage repair preserving other settings. Reference
save failure after formatting is an explicit partial result.

The page remains embedded/offline without a framework or new firmware
dependency. Its larger cost is measured before a later reduction decision:
41,432 B gzip (+28,302 B); C6 flash 1,551,506 B (84.6%), static RAM 197,380 B.
The result fits the existing OTA slot and is about 8.3 KB below the 85% goal.
Thirty-two host logic/markup tests pass, and the actual page was loaded
locally without a device API to check retry visibility. This is not physical
device, gesture, transport, upload or rollback acceptance. The display redesign
has not started; the owner tests this browser stage first.

### OpenLigaDB round and club requests, modem sleep off · 2026-10-12

A full OpenLigaDB refresh no longer streams the whole season (about 300 KB in
October, growing to roughly twice that by May). It requests the current round
and the own club's fixtures around today, filtered to the route's competition
and season, which covers the same retained fixtures as the former season
filter: about 26 KB per refresh including the table. The competition list
requests one season and offers only football entries. The 30-second operation
deadline is unchanged: it limits stalled transfers and how long TLS memory and
the heavy-operation token are held; it is not a rate limit. Station modem
sleep, previously the unchosen ESP-IDF default, is now off on every profile.
Flash +838 B, static RAM unchanged. Device transfer times are not yet measured.

### OpenLigaDB season once a day, change date per round, one-hour maximum age · 2026-10-13

Other clubs' catch-up fixtures outside the current round are wanted in the
Konferenz. OpenLigaDB has no date-range request (API description checked
2026-10-13), so the primary source streams the whole season once per
competition and UTC day (572,116 B for the Bundesliga on 2026-10-13, 1 MiB
body class) instead of that cycle's round and club requests. OpenLigaDB
routes now keep other clubs' fixtures outside the round within 36 hours, as
the other providers already did. A failed
stream drops only those fixtures and is retried in the next cycle. Reused
data is refreshed after one hour at the latest (previously five minutes).
Another round with a retained fixture inside its match window gets its own
change-date check, so a running catch-up fixture is not frozen while the
current round is unchanged. Rejected: one request per other club (many
requests) and fetching every other round in every cycle (more requests).
C6 flash +1,758 B, static RAM +320 B (round marks). Transfer time of the
season stream on the device is not measured.

## ADR-026 — Settings format 4: Konferenz pin switch, matchday without own match · Accepted 2026-10-12 · record extended by ADR-027

**Context:** the owner wants the own match pinned in the Konferenz to be
optional (default on), and the matchday default ("while other matches run")
offered Own match although the own club does not play in that situation; that
choice came with an unreviewed snapshot and had no defined meaning. The Web UI
was completed to the approved design in the same step.

**Decision:**
- Format 4 appends one byte, `pin_own_match` (0/1), after the fixture pairs.
  The record stays positional, so a format change is required. Format-3
  records are read with the flag at its initial value (on); formats 1 and 2
  convert as before. The next explicit save writes format 4.
- `matchday_screen` no longer accepts Own match. A stored value of Own match
  (any format) is read as Konferenz in RAM, without discarding the record;
  the Web UI converts an imported backup the same way before validation.
- The browser OTA gate of ADR-024 accepts format-4 firmware from format-3
  firmware; it refuses the older firmware afterwards (downgrade only by USB,
  which then starts with initial values once format 4 has been saved).
- The switch is stored and offered in the Web UI; the display effect follows
  with the display redesign (implemented with ADR-027).

**Alternative:** append the byte to format 3 without a new number (older
firmware would stay installable by browser, but the format number would not
show the extension).

**Cost and boundary:** model unchanged at 1944 B (the flag fits existing
padding); longest record 1932 B (+1 B transient on save); JSON +21 B. C6 build
including the Web UI completion, against the OpenLigaDB/modem-sleep build
above: firmware.bin +6,928 B, flash 1,559,248 B (85.0 %, about 500 B below the
85 % line), static RAM unchanged at 197,380 B, embedded page 48,481 B gzip
(+6,754 B). The page is sent from flash without a heap copy. Host settings
tests cover the format-4 layout, the format-3 read path and the matchday
conversion; device behaviour is not yet checked.

## ADR-027 — Settings format 5: Konferenz mode, whole matchday, input roles · Accepted 2026-10-13 · crest byte removed by ADR-028

**Context:** the owner decided the Konferenz content (kickoff blocks of every
active competition), two display modes (All pages, One page), an optional
"show own match for the whole matchday" and a free role per input (Off,
Mandatory, Up, Down, Scroll with direction change; exactly one Mandatory).
The previous fixed behaviour per input count and the fixed own-match row could
not express these choices.

**Decision:**
- Format 5 appends ten bytes after the format-4 pin flag: conference mode
  (0/1), whole-matchday switch (0/1) and eight input roles, one byte per input
  slot (`hw::kMaxInputs`). The record stays positional; formats 3 and 4 are
  read with the new fields at their initial values (All pages, off, the
  profile's default roles); formats 1 and 2 convert as before. The next
  explicit save writes format 5.
- Validation requires exactly one Mandatory role among the device's inputs;
  roles of slots beyond the profile's input count are stored but ignored.
  No Mandatory input is reported as required, several as a conflict.
- Default roles come from `cfg::inputRoleDefault(index, count)`: three
  inputs Mandatory, Up, Down; two Mandatory, Scroll; one Mandatory; further
  inputs Up, the third Down.
- The display reads mode, pin and whole-matchday from the model; the input
  policy maps every event through the input's role. The schema reports the
  device's input count (`inputs`) so the Web UI offers one choice per input.
- The browser OTA gate of ADR-024 accepts format-5 firmware from format-3 and
  format-4 firmware and refuses the older firmware afterwards.

**Alternative:** keep the behaviour per input count and add only the two
Konferenz values (one byte less per input, but no free assignment, which the
owner asked for).

**Cost and boundary:** model 1956 B (+12 B: 10 B fields, 2 B padding);
longest record 1942 B (+10 B, transient on save); every `settings::current()`
copy +12 B of stack; JSON +75 B, schema +11 B. C6 build: flash +1,184 B and
static RAM +48 B for the firmware part, +1,536 B for the Web UI fields
(embedded page 50,019 B gzip); total flash 1,563,216 B (85.2 %, 3,459 B above
the 85 % line, permitted during the redesign). Host tests cover the format-5
layout, format-3/4 reads and the role rules; the input mapping has its own
host suite. Device behaviour is not yet checked.

## ADR-028 — Protected stock images as roles, settings format 6 without a crest setting · Accepted 2026-10-13

**Context:** the owner asked for the management of all images under "Images":
the stock crest and the stock background must be replaceable there and can
always be restored to the factory image; the crest is a special image, not a
display setting. Until now the factory files could not be replaced on the
device, and the crest was a settings reference (`crest_image`) to a library
image.

**Decision:**
- Each stock image is a role (crest, background). A role shows its
  replacement file when one exists (`role_crest.jpg`, `role_background.jpg`),
  else the factory file, which is never changed by the device. A replacement
  is uploaded like a library image (same JPEG check) or copied from a library
  image; "reset" deletes the replacement. Roles cannot be deleted. Id 0 in
  every reference means "the role's image".
- Replacements count towards the shared image budget but not towards the ten
  library ids. Deleting all images deletes the library and resets both roles.
- Format 6 removes the crest byte; slide id 0 is the crest role. Formats 3
  to 5 are read as before. Their crest id, when it is a library id, is
  handed to the image service, which copies that image once as the crest
  replacement outside an OTA trial and then saves format 6, so the old
  reference cannot overwrite a later replacement. A missing image or a full
  storage leaves the factory crest; the library image stays. An out-of-range
  crest id is ignored rather than rejecting the whole record.
- The API and the Web UI no longer carry `crest_image`; the Web UI drops it
  from an older backup file before import and says so.
- The API lists both roles with their state and revision, accepts a role
  replacement upload (`X-Image-Role`) and offers copy and reset through one
  endpoint (`POST /api/v1/images/role`).
- The Web UI manages every image on the image page (library uploads, edits,
  deletes; crest and standard background: replace, copy from the library,
  restore). The display page only selects per screen (background off,
  standard background or a library image) and the slideshow order; it has
  no upload.

**Alternatives:** keep `crest_image` and add a separate replacement only for
the background (two ways to change the crest); overwrite the factory files
(no restore without USB upload).

**Cost and boundary:** model 1952 B (−4 B including padding), longest record
1941 B (−1 B); JSON −16 B. C6 build: role service +458 B flash (static RAM
unchanged), format 6 and migration +798 B flash in total with the Web UI
change, static RAM −16 B; the role API +1,310 B flash, static RAM
unchanged, one more HTTP handler; the image page role tiles +1,200 B flash
(embedded page +1,195 B gzip), static RAM unchanged; removing the upload
buttons from the display page −320 B flash; total flash 1,571,628 B
(85.6 %). A replacement
needs its JPEG size of LittleFS within the shared budget. Host tests cover the
format-6 layout and the crest hand-over from format 4 and 5 records; the copy
and the device behaviour are not yet checked on the device.

## ADR-029 — Current season resolved on the device, free-text identifiers · Accepted 2026-10-13

**Context:** the owner decided that every provider follows its current season
automatically and that a stored season stays only as an explicit expert
override. API-Football could only be used with a stored year, so it never
rolled over. Some OpenLigaDB competition shortcuts contain spaces or umlauts
and were neither accepted nor sent correctly.

**Decision:**
- An empty season is valid for every provider and means the current season.
  OpenLigaDB and API-Football need a year in their URLs: the device resolves
  it with one request (OpenLigaDB: the current round's `leagueSeason`;
  API-Football: `leagues?id=…&current=true`, the season marked current) and
  keeps it per competition and source (main or fallback) for six hours; a
  provider-settings change clears it. football-data.org and ESPN get no season
  parameter and answer with their current season.
- The setup wizard stores no season for a picked API-Football competition; it
  passes the listed season to the club list request only, so that list needs
  no extra lookup.
- Competition, club and fixture identifiers are any text without control
  characters and without a space at either end (up to their byte limits).
  Every identifier is percent-encoded (RFC 3986 unreserved characters kept)
  when it is placed into a request URL.
- The list search folds ASCII and the UTF-8 Latin-1 capitals to lower case.

**Alternatives:** keep the stored API-Football year and ask for a manual
rollover (contradicts the owner decision); resolve the season with every
refresh (one more API-Football credit per refresh); restrict identifiers to
URL-safe characters (excludes real OpenLigaDB shortcuts).

**Cost and boundary:** one API-Football credit per competition and source
every six hours while the season is empty. C6 build: flash +428 B in total
(1,572,056 B, 85.7 %), static RAM +128 B (second season cache per
competition), embedded page −282 B gzip. Provider task stack peak estimated (unverified) at about
+250 B on the resolving path (URL buffers); the measured minimum is part of
the provider stack acceptance on the device. Season rollover itself is not yet
observed with real provider data.

## ADR-030 — Request limits per provider, settings format 7 · Accepted 2026-10-13 · spacing, record and request spreading replaced by ADR-031

**Context:** the owner decided that every provider has its own limits, with
defaults taken from the provider's documentation, described and editable by
the user, and that each limit counts only for its own provider. Before, only
API-Football had a stored daily budget; the other providers ran through the
same single counter record with a fixed safety ceiling and wrote to NVS every
eight requests, an unreadable record stopped every provider, and the minimum
request spacing was a compile-time value.

**Decision:**
- Two settings per provider, in provider order (OpenLigaDB, API-Football,
  ESPN, football-data.org): the daily cap (`daily_requests`, 0–65535,
  default 0 / 100 / 0 / 0) and the minimum spacing between request starts
  (`request_spacing_s`, 1–3600 s, default 5 / 6 / 30 / 6). A cap of 0 means
  no daily cap: no counter and no NVS write for that provider.
- Settings format 7 stores both lists (8 bytes each) at the end of the record
  and removes the budget value from the scalars. Formats 2 to 6 read their
  stored budget as API-Football's cap; every other limit keeps its default.
- One 12-byte NVS record per capped provider (namespace `provider_budget`,
  one key per provider) with the same reservation scheme as before (blocks of
  up to eight requests, lost on reboot, a backward clock never reopens a
  day). The single record of earlier firmware is read once for API-Football
  and removed when that provider's first new record is stored.
- A damaged or unreadable record blocks only its provider for the current
  UTC day; the next day writes a new record. The settings reset in the Web UI
  erases every record.
- Every capped provider spreads its remaining requests over the rest of the
  match coverage; the slowest provider sets the poll interval.
- A shorter JSON list in a REST save or import replaces only its first
  entries. The Web UI converts the budget of an older backup into
  API-Football's cap.

**Alternatives:** keep one record for all providers (one damaged record
stops every provider, more NVS writes); keep the spacing at compile time (no
user correction when a provider changes its limits); separate JSON keys per
provider (more schema and validation code than one list per limit).

**Cost and boundary:** model 1968 B (+16 B), longest record 1955 B (+14 B),
JSON +37 B with the defaults. C6 build: flash +2,140 B (1,581,800 B,
86.2 %), static RAM +96 B, embedded page +686 B gzip (56,782 B). The limits
are local: they cannot account for other devices sharing an account or IP
address. API-Football's per-minute limit could not be checked on 2026-10-13.
Host tests cover the format-7 layout, the older-format reads, the counter
rules and the Web UI fields; the NVS behaviour on the device is not yet
checked.

## ADR-031 — Competition store, request plans, settings format 8 · Accepted 2026-10-14

**Context:** the owner wants the display to show the matches of up to eight
of their competitions, the own match in detail, compact data, regular core
data and fixture lists, and request limits that are fully adjustable, both
per day and per time window (for example 60 requests per minute), plus an
optional global limit, without flooding any interface. Format 7 stored three
competitions without identity inside the settings record and one minimum
spacing per provider; fixture pairs referred to the list position.

**Decision:**
- The competitions and fixture pairs move into their own NVS record
  (`competitions`, format 1, at most 2,467 B): up to eight entries, each with
  a stable id 1–255, four screen switches, an optional club, a primary and an
  optional fallback source. Fixture pairs name the competition by id. An
  entry without id gets the next free id; a given id is kept and must be
  unique.
- Per provider a request plan: daily cap (unchanged), window limit of N
  requests per W seconds, and one interval per data type (core data,
  fixtures, table, live, live table, own-match events). One global limit
  (daily cap and window limit) over all providers, default off. Defaults
  follow the providers' published limits; a value of 0 means no limit.
- A fixed 2-second gap between any two request starts of the device,
  independent of the limits.
- Settings format 8 drops the competitions, pairs and spacings and stores the
  plans and the global limit (at most 803 B). Formats 2 to 7 are read in RAM:
  their competitions get the ids 1, 2, 3 in list order and their pairs are
  renumbered; format 7 keeps its daily caps and drops the spacing.
- Loading never writes NVS (OTA trial and rollback stay possible). A
  readable older settings record wins over the competition record at boot;
  the first save writes the competition record first and then the settings
  record in format 8. Reset erases both records.
- Own REST endpoint `/competitions` (GET, POST/PUT replace); export and
  import carry both stores. The Web UI writes the two stores in the order
  that keeps a provider switch valid.
- Until the request scheduler counts exact windows, a window limit is
  enforced as an even spacing (W / N), and the global daily cap and the
  intervals are stored but not enforced. (Historical: since 2026-10-14 the
  request scheduler enforces window limits exactly, the global daily cap
  and the intervals.)

**Alternatives:** keep the competitions in the settings record (a record of
about 2.6 KB, every settings save rewrites all competitions); refer to
competitions by list position (a delete or reorder silently moves fixture
pairs); write the competition record during boot migration (breaks the
"trial firmware writes nothing" rule and can load stale competitions after a
downgrade and upgrade).

**Cost and boundary:** settings model 824 B (was 1,968 B), competition store
2,459 B; JSON `providers` 628 B with defaults (722 B maximum), full
competitions JSON 5,388 B (7,446 B worst case with escapes), below the
8,192 B body limit. C6 build: flash +15,216 B (1,598,148 B, 87.1 %), static
RAM +2,928 B (205,328 B), embedded page +2,671 B gzip (60,035 B). The
migration at boot and the two-record save order are not host-tested (the
host tests cover encoding, decoding, validation and the legacy reads); the
device check that a format-7 device keeps its competitions after the update
is open. A save that writes the competition record and then fails on the
settings record leaves the old settings record, which still wins at the next
boot.

## ADR-032 — Kept football data and own short names on LittleFS · Accepted 2026-10-14

**Context:** the owner wants data shown quickly and few requests, with very
little memory. Without persistence every reboot showed nothing until the
next requests and repeated the core data, fixtures and table requests. The
owner also wants to name teams themselves, across all competitions.

**Decision:**
- One binary file per active competition (`comp_<id>.bin`): header with a
  build layout id and the competition's source key, its part of the match
  store, what its sources resolved and the UTC time each kept data type
  last completed, CRC-32. Written atomically (temporary file and rename)
  only for persistence: when the worker is idle, at most every 600 s per
  competition, never while one of its matches is in its live window.
- Loaded at boot for active competitions with unchanged sources; the
  request scheduler then waits for the rest of each saved interval. A file
  of another layout or source key is ignored and deleted.
- One file of the owner's short names (`short_names.bin`), keyed by provider
  and team id, at most 48 names of 26 bytes, applied when a view reads a
  name; competition files keep only the provider's names.
- Football files together at most 48 KiB; images come first, so a write
  leaves the unused part of the image library quota (whole blocks) plus
  the image reserve free. The first boot of an update writes none.
  Settings reset deletes them; image repair rewrites them.
- The settings export carries the short names; an import replaces the
  whole table with one file write (`PUT /teams`).

**Alternatives:** JSON files (a parser run and a larger file per load);
several files per competition (more blocks and more partial states); short
names inside the competition files (lost with a changed competition);
writing during live play (flash wear for data that change every minute).

**Cost and boundary:** C6 build flash +12,030 B (1,624,064 B, 88.5 %),
static RAM +2,128 B (150,960 B). An 18-team league with an own match takes
2,947 B (one 4 KiB block, host measurement); a full short-name file
1,552 B. The last minutes before a power loss can be missing. The device
evidence (data after a reboot without a request) is open.

## ADR-033 — Matchday screen, screen switches per competition, table cycle, ESPN substitutions · Accepted 2026-10-14

**Context:** the owner wants to see "nur den letzten bzw. aktuellen
spieltag und die tabelle aus liga 2 und 3", wants competitions usable only
for chosen screens ("Schalter je Wettbewerb"), a table that pages by itself
without an own club ("Automatisch seitenweise"), a live table during
matches, and ESPN substitutions "Live alle 10 min + nach Abpfiff". Until
then the first active competition supplied the only table shown, and the
scoreboard named substitutions only as "A / B".

**Decision:**
- A fifth screen, Matchday (stored index 4; display order Own match,
  Konferenz, Matchday, Table, Crest, published as `screen_order`). It shows
  each Matchday-enabled competition's first stored matchday (current until
  it expires, then the next) in pages of `visible_matches`, then the next
  competition in settings order.
- Each competition has four screen switches (Own match, Konferenz,
  Matchday, Table). The request plans fetch only the data types the
  switched-on screens need; each screen reads only its competitions.
- The Table screen cycles through the Table-enabled competitions with a
  stored table; without an own club it pages `table_window` rows every
  `table_page_s` (8 s, 3–600 s). The live table is requested for these
  competitions while one of their matches is live.
- The Matchday screen's style and `table_page_s` are a trailing tail of
  settings format 8 (no new format): an older format-8 record keeps their
  defaults, older firmware ignores the tail.
- An ESPN primary with a club and Own match on requests the match summary
  of its started own match at the events interval and once after the live
  window ends; only the `keyEvents` substitutions are read (player in and
  out), stored as their own event origin, with a 512 KiB body cap for this
  request.

**Alternatives:** a new settings format 9 (one more migration path for 20
bytes); a Matchday view inside the Konferenz (mixes live blocks with whole
matchdays); keeping one table competition (the owner's 2. and 3. Liga
tables would not both show); the ESPN core API `plays` for substitutions
(1.5 MB for 1000 plays, larger than the summary).

**Cost and boundary:** C6 build flash +5,270 B (1,632,710 B, 89.0 %),
static RAM +152 B (151,112 B), embedded page +1,074 B gzip (62,843 B);
settings model 844 B, longest record 821 B. The summary is about 400 KB per
request and is read in full (only `keyEvents` is parsed into elements); whether it arrives within
the 30 s request deadline and the provider stack reserve with it are not
measured on the device. Whether ESPN and OpenLigaDB change their tables
during a match is not yet checked on a match day.
