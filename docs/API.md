# Device REST API

The current development API uses one coherent application configuration.
`GET /config` reads its revision; `PUT /config` applies settings, competitions,
display preferences and optionally adopted names together. Development formats
start fresh; no older mutation routes or backup migration are provided.
All JSON uses one bounded streaming grammar and encoder, without a document
allocation. Host/build acceptance is separate from browser/device acceptance.

## Endpoints

| Method and endpoint | Purpose |
|---|---|
| GET `/api/v1/status`, `/schema` | Status, screen/competition state, build identity, heap/task minima and bounds/options/upload limits |
| GET `/api/v1/session` | `{password_set, valid}` for the token the request carries |
| GET `/api/v1/wifi`, `/selection?query=ID` | Bounded scan or exactly correlated selection results |
| GET `/api/v1/teams?competition=ID` | RAM team IDs and compact provider fallback labels; no adopted-name I/O |
| POST `/api/v1/team-names/originals?competition=ID` | Protected explicit full-original name query using the existing worker; changes no draft or stored names |
| GET `/api/v1/team-names/preset` | Complete immutable club-name source belonging to the running app; does not adopt names |
| GET `/api/v1/images`, `/images/file?id=N\|stock=crest\|background[&r=rev]` | Image inventory: library images with per-image revision, and the two roles `crest` and `background` with `replaced`, replacement `bytes` and `revision` (it changes when the role's image changes); the JPEG itself (`stock=` delivers the role's replacement, else its factory image) |
| POST `/api/v1/session`, `/logout` | Open/close the mutation session |
| GET, PUT `/api/v1/config` | Coherent redacted configuration / one protected Apply |
| GET `/api/v1/team-names` | Adopted collection and exact names reference; no changes |
| POST `/api/v1/export` | Protected coherent current-version configuration and complete adopted collection export |
| POST `/api/v1/wifi/scan`, `/data/select`, `/data/refresh` | Start asynchronous scan, provider list or refresh |
| POST `/api/v1/data/select/cancel` | Cancel only the named selection query |
| GET, POST `/api/v1/demo` | Read scenario catalogue, current step/test time and runtime competition bindings; select a scenario/step while saved Demo mode is enabled |
| POST `/api/v1/display` | Display commands `next`, `ip` and Demo-only `auto_next` |
| POST `/api/v1/screenshot` | Authenticated forced redraw streamed as raw RGB565 strips; the browser creates the PNG |
| POST `/api/v1/images/upload`, `/images/delete` | Upload a library image (optional `X-Image-Id` replaces or restores that valid library id; `X-Image-Role: crest\|background` uploads the replacement of that role instead, answer `{"ok":true,"role":…}`) or delete a library image by id |
| POST `/api/v1/images/role` | `{"role":"crest"\|"background","action":"reset"}` deletes the replacement, so the factory image shows again; `{"role":…,"action":"copy","id":N}` copies library image N as the replacement (the library image stays). Roles cannot be deleted. A replacement counts towards the image budget, not towards the ten library ids |
| POST `/api/v1/ota` | Stream compatible firmware to the inactive slot |
| POST `/api/v1/reboot`, `/reset` | Reboot or reset settings and adopted names, optionally delete all images or format image storage |
| WebSocket `/api/v1/debug?token=` | Controlled live status while the Debug tab is open; ends with the session that opened it |

After the first full path, shortened paths in the table retain `/api/v1`.
See [WEB_UI.md](WEB_UI.md) for the browser workflow and image contract.

For a new automatic library upload (no `X-Image-Id` or `X-Image-Role`),
the device chooses the lowest id with neither a file nor a saved screen or
slideshow reference. Optional `X-Image-Reserved` adds the current browser's
unsaved references as a decimal bit mask: bit N reserves library id N.
Valid bits are 1…10 (mask 2046); zero is allowed, bit 0 and other bits are
rejected. This header conflicts with an explicit id or role. Missing header
means no additional browser reservation; saved references are always
reserved. No available id uses the existing capacity error. Explicit
`X-Image-Id` intentionally replaces/restores that id, irrespective of its
references. Firmware uploads ignore image headers, as before.

## Endpoint details

### Images

Upload and library-to-role copy write one temporary candidate, check every
read/write/sync/close, validate its actual JPEG bytes and rename only after
success. The old file remains published on failure. Physical admission uses one
shared replacement workspace; the library quota remains independent.

`POST /images/delete` returns these phase fields on both success and service
failure: `references_cleared`, `settings_saved`, `settings_revision`,
`file_deleted`, `already_missing`, `outcome_unknown`. A failed unlink can follow
a successful reference save; `settings_saved:true` does not claim file deletion.
An uncertain configuration write sets `outcome_unknown:true` and skips unlink.
Reload/reboot to establish the durable state before another mutation; never
automatically resend that deletion. Read-only inventory reconciliation is safe.

Reset partial-failure responses also include `image_files_deleted` and
`image_files_already_missing`, counting confirmed per-file outcomes before the
first failure. They do not imply that the whole reset completed.

### Application configuration and Apply

`GET /config` returns `{version,revision,apply_id,desired,names_ref}`. `desired`
contains `settings`, `competitions` and `screens`. Secrets are redacted: presence
flags are readable; values are returned only by protected explicit export with
`{"secrets":true}`. Revision and names reference come from the same short lock.
The 64-bit `apply_id` is a fixed16-character lowercase hexadecimal string,
preventing browser integer precision loss. Zero denotes no prior Apply.

`PUT /config` requires `{expected_revision,apply_id,desired}` and optionally
`adopted_names`. A new Apply ID is nonzero. Every section of `desired` is
required. Omitted secret fields retain their current values; explicit empty
strings clear them. Raw SSIDs support UTF-8 strings (including escaped NUL)
or the exact byte-preserving `{base64:"…"}` representation. Field and cross-
section validation occurs before one NVS publication; the config owner assigns
new competition identities. `settings_changed`409 rejects a stale revision.
An OTA trial rejects mutations. Unknown/duplicate keys, malformed Unicode,
wrong types and trailing input are rejected before publication.

Changed names are validated in an inactive immutable file. One NVS record then
selects its exact slot/generation/length/CRC. A settings-only Apply does not
open or rewrite names. A successful answer includes `ok:true` and the complete
redacted configuration receipt, including assigned IDs and activated revision.
Network reconfiguration waits for the admitted operation to release its shared
operation slot. There are no section commits, client write ordering, generation
arithmetic or partial configuration successes.

A lost response never authorizes resending Apply. Read `/config` and compare
`apply_id`. `outcome_unknown`503 means NVS may already be durable: both name
versions are retained, old coherent RAM remains, further saves are refused
until reboot/recovery. Reading old RAM before reboot does not prove absence.
The browser retains the complete draft and provides a read-only status action.
Acknowledged writes are not repeated when a subsequent read is busy.

The schema provides separate `max_body` and `max_apply_body` transport ceilings;
these are not RAM buffers. One shared pull reader consumes bounded chunks.
`POST /export` returns `{version,desired,adopted_names}` from a coherent snapshot.
Current-version import loads the whole result into the common browser draft;
only Save applies it. Older backups are deliberately unsupported before alpha.

### Screen programmes and competition preferences

`desired.screens` owns common type order, five shared phase rule sets,
appearance, slideshow IDs, timing windows, image-frame mode and page options.
Phases are `own`, `other`, `matchday`, `idle`, `break`; screen types are
`single`, `conference`, `table`, `crest`, `matchday`, `images`.
Shared tuples are `[enabled,automatic,visit_s]`; `per_competition` activates
attached sport overrides for a phase. Each competition owns its five-by-four
sporting override tuples under `display`. Each tuple field may independently
be `null` to inherit the shared value. Explicit false stays explicit; collapsing
individual rules retains them for later restoration. There is no independently
maintained association identity matrix or separate screen generation.

A competition's `primary` owns provider/competition/season. `club` separately
owns numeric preferred team, selection label, own-special flag and explicit
group ID. Group0 is ungrouped; equal nonzero group IDs link selected competitions.
Names and catalogue imports never change this membership. The highest-priority
member determines block placement and own focus. Options and bounds, including
hardware backlight capability, are published by `/schema`; no browser default
copy replaces the configuration model. `overview_screenshot` remains a general
boolean: it adds the Overview trigger; Debug screenshot remains available.

### Runtime Demo

`GET /demo` returns `enabled`, `pending`, `scenario`, `step`, `revision`, `utc`,
`bindings` and `scenarios`. Every catalogue entry contains numeric `id`, stable
`key`, title and ordered step labels. Bindings identify dataset0/1, canonical
`competition`, `incarnation`, and whether that association came from saved
competitions. The first two saved entries supply order and screen rules. A
missing saved association uses common rules and is explicitly labelled.

`POST /demo` accepts exactly `{"scenario":N,"step":N}` with catalogue bounds,
normal mutation session/origin checks and no OTA trial. First enable and save
Demo in general settings; otherwise409 `demo_disabled`. A queued command returns
`{"accepted":true}`; this is not completion. Poll until `pending:false` with the
requested scenario/step and new publication. Busy commands return409. Store,
clock, bindings and phase proof publish together. These controls never save
scenario data, modify provider sources, write real caches or set system time.
The scenarios exercise canonical selection and rendering; provider authenticity
and physical acceptance require their separate checks.

### Screenshot download

`POST /screenshot` accepts the usual JSON object body (`{}`), same-origin
check and session token. With no admin password, the Web UI opens the
existing silent session. The live-debug setting does not gate screenshots.
Capture is also available during an OTA trial; it does not change settings
or files. Shared heavy admission rejects competing work with 409 `busy`.

The device invalidates the active screen once and copies its next real LVGL
output before normal LCD byte swapping. The response is an uncached
`application/octet-stream`, with `Connection: close`. It describes the
current logical reading orientation, rather than a fixed case orientation.
This redraw is not LCD RAM readback or evidence of an optically stuck panel.
The display can pause during capture, bounded to five seconds on the C6.

All integers below are unsigned and little-endian. The version-1 header is
12 bytes:

| Offset | Bytes | Meaning |
|---|---|---|
| 0 | 4 | ASCII `FBS1` |
| 4 / 6 | 2 each | Logical width / height; odd quarter-turns swap the profile dimensions |
| 8 | 1 | Rotation, 0…3 quarter-turns; descriptive, no extra client rotation |
| 9 | 1 | Shape: 0 rectangular, 1 round |
| 10 | 1 | Pixel format: 1 = RGB565 little-endian |
| 11 | 1 | Maximum rows per strip, currently 20 |

Each record has four 16-bit values (`x`, `y`, `width`, `height`), then
`width × height × 2` compact pixel bytes without source stride padding.
Rectangles need not be full-width or arrive in coordinate order. Eight zero
bytes terminate a successful capture; no bytes follow that record. Clients
must reject missing/overlapping pixels, invalid dimensions, truncated or
trailing data and an absent terminal before creating an image. The browser
expands RGB565 and masks pixels outside the round display to transparent
alpha, then downloads `fussball-YYYY-MM-DD_HH-MM-SS.png` in local browser time.
It stores no screenshot file on the device.

Before streaming, errors use the normal JSON body: 503
`screenshot_unavailable` for an unavailable/unsupported display, 507 `memory`
for insufficient capture RAM, or the existing admission/timeout errors.
After streaming starts, a failed capture/send closes the connection without
a success terminal or appended JSON. Every partial underlying socket send
shares the same absolute capture deadline. The original LCD transfer and
flush-ready ownership remain unchanged on success, timeout and disconnect.

### Display actions and status

`POST /display` requires an `action` string and `config_revision`, the uint32
revision returned by the caller's last `GET /config`:

| Body | Effect |
|---|---|
| `{"action":"next","config_revision":12}` | Existing manual Next: choose the next manually reachable sporting entry, or browse the next frame image while frame mode is active |
| `{"action":"ip","config_revision":12}` | Existing temporary IP-address display |
| `{"action":"auto_next","config_revision":12}` | Demo only: while manual, first return to automatic display; otherwise advance one existing automatic boundary, through subpages/images before the next visit/type/competition |

All three use the common origin, session and JSON-body admission. The body is
streamed once into a typed command with bounded storage and no JSON document.
Only `action` and `config_revision` are allowed; duplicate keys (including
escaped spellings) and unknown keys are rejected before a command can be
queued. Like the existing quick actions, they remain available during an OTA trial. A missing
or null `action` returns HTTP 400 `required`; another non-string value returns
`type`, and an unsupported string returns `invalid`, all with `field:"action"`.
When runtime Demo is disabled, `auto_next` returns HTTP 409 `demo_disabled`;
the controller also rejects that action outside Demo.

Missing/null `config_revision` returns HTTP 400 `required`; a non-integer
JSON value returns `type`, and values outside 0…4294967295 return `range`, with
`field:"config_revision"`. An old revision returns HTTP 409 `settings_changed`
without queueing or relabelling the action. Refresh the configuration before
issuing a new command; do not retry it automatically.

Commands retain that caller revision through the one controller FIFO. A
successful settings/competition save discards earlier commands, including
delayed delivery, while preserving commands issued for the new generation.
Physical gestures already in progress are cancelled too. A full action queue returns HTTP 409 `busy`. Success returns `{"ok":true}` for queue
admission, not command execution or LCD presentation. An output still awaiting
its first presentation is not skipped by `auto_next`.

`auto_next` changes runtime navigation only. It preserves saved visit/image
times, eligibility, competition bindings and the selected Demo scenario/step
and test UTC. It does not start recurring advancement; send another command
for another boundary. `POST /demo` remains the separate scenario/step action.

`GET /status` exposes these controller fields:

| Field | Values and meaning |
|---|---|
| `screen` | Stable numeric type: `0` Single, `1` Conference, `2` Table, `3` Crest, `4` Matchday, `5` Images; independent of editable sequence order |
| `screen_reason` | String: `setup`, `manual`, `own_match`, `matchday`, `idle`, `other_games`, `break` or `frame` |
| `screen_starting` | Boolean: startup-card state in the bound controller snapshot |
| `screen_manual` | Boolean: the bound navigation state is manual |
| `screen_frame` | Boolean: the bound navigation state is in picture-frame mode |
| `screen_phase` | Numeric phase: `0` = `own`, `1` = `other`, `2` = `matchday`, `3` = `idle`, `4` = `break`; setup/frame are represented separately, not extra phase numbers |
| `screen_automatic_entries` | Number of eligible automatic type/competition entries, computed when the controller binds in Demo outside frame mode. Subpages are not separate entries. Other controller binds write `0` because the count is not computed |

These controller fields come together from one atomic last-bound/poll snapshot.
They do not acknowledge a completed LCD transfer, and may describe output still
pending presentation. They are read separately from the football overview;
the complete status JSON is not one transaction across those two sources.

`GET /status` includes `network_init` alongside the existing connection
state and IP addresses. These are separate facts: working WiFi can remain
connected while a local naming or setup-DNS service is being recovered.

| Field | Meaning |
|---|---|
| `ready` | Every required local network initialization phase is confirmed; Internet and SNTP availability are not required |
| `step` | Machine key for the first currently failed phase; no failure when empty |
| `error` | SDK error value for that phase, zero after recovery |
| `error_name` | SDK error name, such as `ESP_ERR_NO_MEM`; `ESP_OK` without a failure |
| `code` | Short diagnostic code `NET E01` through `NET E13`, empty without a failure |
| `retry_ms` | Earliest monotonic device milliseconds for the next retry; zero without a pending failure. Compare with `uptime_s`, not UTC; an admitted heavy operation can delay execution |

The phase keys are `antenna` (antenna switch), `network_stack` (TCP/IP and
default event loop), `station_interface`, `ap_interface`, `ap_address`
(setup address/DHCP), `wifi_driver`, `wifi_configuration`, `wifi_events`,
`radio`, `portal_dns` (setup-DNS responder), `mdns` (local name service),
`hostname` (current DHCP/mDNS name) and `http_service` (local HTTP
advertisement), in code order E01–E13. Each retry retains successful
resources; a failure in one independent service does not restart working
WiFi. After recovery the active error clears. The local OTA trial checks the main loop, display and watchdog. Network,
HTTP, DNS, SNTP and provider failures remain retryable service faults and do
not invalidate otherwise healthy firmware.

`GET /status` also includes one coherent `image` object for the currently
selected display image. It describes content, independently of UI/hardware
health and firmware confirmation.

| Field | Meaning |
|---|---|
| `active` | The current screen requests an image; off/start screens clear it |
| `pending` | File selection/check or LVGL source acknowledgement is pending; an already usable fallback can remain visible |
| `id`, `role` | Desired library id, or id 0 for the protected `crest`/`background` role; automatic fallback never changes this preference |
| `actual` | Fully validated selected file kind: `library`, `replacement`, `factory`, or empty; while pending it is not yet acknowledged by LVGL |
| `failed` | File kind that caused the current error; an exhausted chain reports its final failed candidate |
| `reason`, `error`, `error_name`, `code` | Content failure key, SDK-style error value/name, and short `IMG E01`…`IMG E07`; empty/zero without a failure |
| `revision`, `attempt` | File revision and full-width source-attempt identity; neither proves completed LCD output |
| `retry_ms` | Earliest monotonic device milliseconds to retry the desired file, zero without a scheduled failure; heavy-operation admission can delay it |

Image reasons are `missing` (E01), `unavailable` (E02: a file/decoder setup
could not be read/prepared), `invalid` (E03: full-file validation rejected
format/dimensions/content), `timeout` (E04), `source` (E05: LVGL did not
acknowledge the source), `decode` (E06: later block delivery failed), and
`memory` (E07). A late header/open failure alone does not prove a corrupt
file; for example the decoder's temporary allocation can fail with the same
return value. The next full validation can refine that diagnosis.

The full JPEG checker distinguishes rejected content from confirmed file
read, seek or close errors. Its current deadline takes priority, including
when I/O used the remaining time; an already expired deadline avoids file
I/O entirely. This does not impose a deadline on the SDK's later draw calls.

`GET /images` reads all twelve writable image slots once before sending its
inventory. Only an absent file is reported as absent; an unreadable metadata
entry rejects the complete request, so it cannot invent free quota or a factory
role. The browser retains its last valid inventory and unsaved references.

`GET /images/file?display=1` serves the currently selected actual file,
including its factory fallback while a damaged replacement still exists.
It returns the existing missing-file error when no candidate is selected.
This dynamic response always uses `no-cache`; an optional `r` cache-buster
does not make it immutable. Existing `id=N` and `stock=crest|background`
downloads retain their original raw-library/role choice and revisioned
immutable caching. Role downloads choose the factory file only when the
replacement is absent; metadata/open I/O errors remain server errors. A read
or file-close error aborts the chunk stream without a normal
completion marker, rather than reporting a truncated JPEG as complete.

`GET /wifi` returns `busy` for a queued or running user scan and an
`error_code`: empty after success, `scan_start`, `scan_result`, `net_lost` or
`timeout` on failure. The list and state are read together. Connection scans
have priority; a user scan runs with the existing 15-second scan deadline.
Duplicate user requests return 409 `busy`; a failed or cancelled job is never
represented as a successful empty scan.

`POST /data/select` accepts the typed provider/competition/club-list request
and returns `{"ok":true,"query":"1234567800000001"}`. The opaque16 lower-case
hex digits preserve all64 identity bits in JavaScript. Its boot nonce and
positive sequence distinguish this operation from another browser or reboot.
An explicit busy rejection owns no query; an unacknowledged start is never
resent automatically.

`GET /selection?query=1234567800000001` returns that same `query`, `busy`,
`items` (`id`, `name`, `season`; at most one page), `more`, `revision` and
`error_code`. Missing/malformed IDs are rejected; a superseded, cancelled,
expired or configuration-stale ID receives409 `changed` before output. Row
reads recheck both query and result revision; replacement during streaming
aborts without a final success terminator. It cannot return another query's
successful list. Provider failure also supplies its cause/code/HTTP status.

While busy, `wait`/`wait_s` describe a window limit or provider pause. Matching
reads renew the30-second idle lease, so an open editor can retain a long limit
wait. An abandoned/lost-start query expires without owner polling. Automatic
work remains eligible between query requests; a request does not hold heavy
admission while waiting for a limit.

`POST /data/select/cancel` requires `{"query":"1234567800000001"}` and normal
mutation authentication. Only that query can be cancelled. An active provider
request retains the single interactive slot until its worker acknowledges
cancellation; late rows/completion cannot overwrite a replacement. Closing the
editor by its header, button or Escape cancels its known ID; closing before
start acknowledgement cancels the eventual returned ID. Results never update
a closed or replaced editor. Each additional page starts its own correlated
request using the previous list's source/search parameters.

The schema publishes `ota_max` from the inactive OTA slot, `selection_text_max`,
`config_version`, `max_apply_body`, the names format/source/wire versions and
normalized file/text bounds. `operation_deadline_ms` and `socket_timeout_ms`
come from the same transport configuration as the explicit originals query.
`firmware_id`
in status is the full 64-character lowercase hexadecimal ELF SHA256 from the
app descriptor. It distinguishes builds with equal version strings; it is
not the shortened hash printed by the panic handler.
The schema's `screen_policy` names six stable types. Their status identities
and controller snapshot semantics are listed under
[Display actions and status](#display-actions-and-status).
Competition ids/counts in status come from the same published canonical data,
including Demo routes, rather than a second settings-index lookup. `demo`
identifies this runtime projection. Demo rows additionally expose `bound` and
`incarnation`; append saved labels only for exact bound identity. The football
overview's own-match and competition fields share one repository read, including
inactive rows; controller fields use the separate snapshot described above.
Old `competition_screens`, general `screens`
and `table_page_s` are removed.
Status `requests` contains each provider's request attempts since boot,
including selection requests. It is not a daily usage or remaining-credit
counter and must not be compared directly with the configured daily caps.

Full LittleFS formatting is available only in the existing explicit factory
reset (`POST /reset` with `confirm:"reset"` and `format_storage:true`). There is
no standalone repair route or pre-alpha compatibility alias.
Settings and request counters are reset before formatting. Formatting also works
when the filesystem cannot mount; it performs the authorized file deletion.
Only complete success schedules a restart. A failure returns 500 `reset_partial`,
its failed `stage` and flags describing completed destructive steps; retry the
confirmed reset to finish. Adopted names are intentionally deleted and the
immutable app preset survives. There is no RAM/NVS names-restoration rewrite.
Image-only deletion remains a separate operation.

Status `fs_total` and `fs_used` cover the whole LittleFS partition: images,
stock files and adopted names. Football data is RAM-only.

## Club-name firmware source

`GET /team-names/preset` returns the complete `football-team-names` version1
source JSON, including original long DE/EN/short texts, keys, league metadata,
all provider-ID arrays and aliases. `X-Team-Names-Source` is the SHA256 of the
maintained source file used to build this running app. The JSON values
round-trip; whitespace/field order need not match that file. USB/OTA and
rollback select the matching app source automatically, without `uploadfs`.

This read never saves or adopts any name. It takes the existing heavy-operation
admission before streaming and returns retryable409 busy when unavailable.
The response uses `application/json` and `Cache-Control: no-store`, with no
whole-catalogue DOM or filesystem copy. Transport failure or the existing
absolute request deadline aborts a partial stream; no JSON error is appended.
## Adopted club names

`GET /team-names` returns `{names_ref,adopted_names}` for the exact referenced
immutable collection. `GET /team-names/preset` returns the complete read-only
human source catalogue; reading it never adopts names. Runtime provider labels
are compact original26/3 fallbacks, not a full originals archive. Visible labels
are resolved outside the football lock; full-name and short-name overrides are
independent, including explicit empty strings.

The collection retains DE/EN, short code, league metadata, aliases, all four
providers' ID bindings and independent provider/ID exceptions. Source version1
uses string fields. Adopted version2 uses one `texts` dictionary with positive
collection-local IDs; club `de/en/short/league`, aliases and exception names
reference those IDs. The dictionary avoids multiplying a shared long alias in
JSON. IDs are unique and referenced; missing or unused entries are rejected.
Names already adopted in version2 obey26/3 UTF-8 byte limits. Source version1
validates complete original strings before clipping at a Unicode boundary.
Empty present fields remain distinct from absent fields. Metadata is retained
in full within the existing16KiB normalized file envelope.

Both this read and a complete settings backup prepare the referenced collection
and its bounded grouping index before any HTTP bytes. Allocation failure is a
normal507 `memory` response; metadata/read failure is an error, never an empty
successful collection. Preparation holds one immutable file handle through
streaming. Each binding/alias is visited a bounded number of times, independent
of club count. The file is checked closed before the normal HTTP terminator;
late I/O/send failure closes the incomplete response without appended JSON.

The browser resolves text references for editing and deduplicates them again
for Save/export. Collection import, catalogue selection and short exceptions
change only the shared draft; there is no immediate write endpoint. Full
collection replacement is available even when the old referenced file is
unreadable, subject to usable NVS and mounted storage. No names operation
regroups competitions or matches clubs by equal text. `POST /team-names/originals?competition=ID` with `{}` returns
`{query,source:{provider,competition,season},items:[{id,name,short_name?}]}`.
It uses that saved source, resolves an automatic season when required and
streams complete decoded names. It shares the selector's interactive worker
slot, actual first-send budget/window gate and one absolute30-second operation
deadline. There is no automatic adoption, archive or alternate HTTP engine.
Closing/failed sends cancel ownership; a partial/malformed provider reply closes
the response without a successful terminator. No JSON error is appended after
a partial body. Only a fully received/parsed response may enter a browser draft.
Compact `/teams` fallback labels are never advertised as originals.

Status competition entries also carry `datasets:{matches,table,details}` from
one coherent RAM snapshot. Each dataset has `state` (unknown/partial/complete/
unavailable), `last_attempt_ms`, `last_success_ms`, `cause`, `http` and an error
`code` when needed. Timestamps are64-bit monotonic milliseconds; an actual
HTTP first-send advances attempt time. A successful fixtures request cannot
clear an earlier table/details failure or overwrite its success time. The
competition summary is derived from those independent facts. JSON is sent
after releasing the football mutex; the HTTP snapshot costs576 transient bytes.

## Competitions

`desired.competitions` is the ordered configuration list, at most the schema's
competition limit. The configuration service assigns IDs and incarnations to
new entries. Each entry contains `id`, `incarnation`, `active`, `primary`,
`competition_name`, `club` and `display`. Provider indices correspond to
`schema.provider_keys`. The source is `{provider,competition,season}`; the
separate club is `{team,name,own_special,group}`, with numeric team0 meaning no
preferred club. Changing a club keeps unchanged source data and reclassifies
own matches; changing provider/competition/season invalidates that source.

Nonzero equal group IDs express membership chosen in the editor; there is no
name-based linking. Group blocks use the highest-priority member position;
members retain configured order. Screen overrides belong to the entry itself,
so reusing a small ID cannot inherit a removed entry's independent matrix row.
All changes are validated together with provider access and screen settings by
one Apply; there is no separate competition write endpoint.

## Status fields

`GET /status` reports internal 8-bit memory from one observation by the health
service's shared heap coordinator. `heap_free` and `heap_largest` are current
point values in bytes. `heap_min` is the SDK's sum of regional free-heap minima;
the regions need not have reached their minima at the same instant. Its scope
is explicit:

| Field | Meaning |
|---|---|
| `heap_min_scope` | `lifetime` since boot, `interval` for the currently owned measurement interval, or `unavailable` during an unstable/failed observation |
| `heap_min` | Minimum bytes for that scope; JSON `null` when unavailable |
| `heap_min_start_ms` | Monotonic start of an interval; 0 for lifetime or unavailable scope |

`tasks` contains `{name, id, stack_min_free, stack_usable_bytes}` for all
measured tasks, including SDK tasks (at most 16). `stack_min_free` is the
lowest free stack in bytes since task creation. `stack_usable_bytes` is the
inclusive span between the kernel's stack bounds after port alignment, not
the originally requested allocation: a 12,800 B C6 allocation has a 12,785 B
usable span. Zero means the usable capacity could not be read safely. On SMP
targets the name stays empty and usable capacity is 0; copied IDs and free
minima remain available. A failed, incomplete or over-capacity task snapshot
returns an empty array rather than a partial list.

`app_bytes` is the cached length of the actual running image, parsed at boot
from its flash partition with the SDK image-metadata reader. It includes image
headers, segment data, checksum alignment and an appended hash. The current
targets use unsigned images; the metadata length does not include signature
blocks and must not be treated as complete signed-image occupancy. An invalid
or unreadable length is JSON `null`. This is neither ELF size nor whole-slot
capacity, and metadata parsing is not an image-integrity verification.
Existing `/schema` `ota_max` is the actual inactive OTA slot's capacity; the
supported layouts have equal application slots, so it is also the capacity
used to judge the running image's occupancy.

The integration acceptance limits are at least 40,960 B free-heap minimum,
32,768 B largest free block, 25% free of each task's usable stack capacity,
and at most 85% application-slot occupancy. Interval/unavailable heap minima,
unknown stack capacity or unknown image length do not establish acceptance.
Largest-block point samples cover only their observation times, not an
unobserved continuous minimum. Exposing these fields does not prove that the
limits hold under the required device workload.

`settings_source`, `competitions_source` and `screen_policy_source` report the
same aggregate configuration owner: `stored` (the stored record or last save),
`initial` (no record, or after reset), `outdated` (an unsupported record version),
`rejected` (a decoded value outside its limits), `damaged` (wrong length or not
decodable), `unreadable` (NVS read error or no memory), `no_storage` (NVS unusable)
or `outcome_unknown` (a mutation may already be durable). There are no separate
competition or screen records and no older-record fallback.
Except for `stored` and `outcome_unknown`, these states use initial values.
An `outdated`, `rejected` or `damaged` record can be replaced by the next save;
`unreadable` and `no_storage` refuse saving until reset/recovery.
`outcome_unknown` retains the previous coherent RAM configuration, blocks further
writes and requires reboot/recovery to establish the durable record.

`competitions` has one row per competition in list order: `id`, `matches`
(match count in the published data), `provider` (source of those matches)
and `cause` (a cause key below; empty while the competition is fine). With
a cause the row adds `code` (short error code), `error_provider` (the
provider of the failed request or of the unmaintained matches) and `http`
(the HTTP status it answered with, only when there is one). `missing`
lists what of the competition the device could not hold, whatever the cause
shown: any of `matches`, `table`, `teams` and `events` (absent when
everything fitted).
`last_update_ms` is the monotonic time of the competition's last successful
request (absent before the first one). `own_match.competition` is the id of
the competition of the club's current or next match. `provider_started` is
false until the provider service runs; the Web UI then shows "loading"
instead of an error.

`last_update_ms` is the monotonic time of the oldest last successful
provider request over the active competitions (0 = none yet). After reboot no old provider data is restored; zero means no successful request
in the current boot. `data_stale` is true while an active competition has a
partial dataset or retains a dataset request failure. It is also true while a
shown match is in its live window and the last successful match request of its
competition is missing or older than the stale limit.
A competition with an unresolved dataset failure has a `cause`; it keeps its
earlier data until that dataset succeeds, while other datasets and competitions
can update. A fixtures success cannot clear a table or details failure.
`unmaintained` is a state, not a failed request: the provider has left
matches without a result for more than 24 hours after their kickoff.
`store_full` is a state as well: the requests succeeded, but not all data of
the competition fitted the device's store (`missing` names which). The
device then keeps the matches nearest in time and every tracked-club match
that fits, keeps a stored table until a new one fits whole, and keeps the
events of the club's live and next matches first. Each kind of data clears
its own entry: `matches` (and the match teams) after a complete fixtures
refresh, never after a live request alone; `table` (and the table teams)
when a new table replaces the stored one; `events` when no tracked-club
match of the competition still lacks events for lack of space. A failed
request shows its own cause instead; `missing` is still reported.

`next_request_ms` is the monotonic time the request scheduler wants to run
the next provider request (compare with `uptime_s`; 0 = none scheduled) and
`next_request_reason` says why nothing runs before it: `live` (a shown match
in its live window is running), `prematch` (a live window is open before
kickoff), `postmatch` (a match in its live window has finished), `idle` (no
live window open; the next data type falls due by its interval), `window`
(the next live window opens at that time), `budget` (a daily cap stretches
or skips data types), `budget_used` (a daily cap is used up; requests resume
after 00:00 UTC), `limit` (a window limit of a provider or the global limit
delays the request), `retry` (a failed request is retried with backoff, or
the provider answered "too many requests" and is paused), `waiting` (no
network or no valid time; `next_request_ms` 0), `update_test` (paused while
a new firmware is in its update test; `next_request_ms` 0) and `none`
(nothing scheduled yet after a start). The intervals per provider and data
type are the request plans of the settings. A refresh from the Web UI makes
every data type due at once; the limits still apply.

`network` is the WiFi state (`idle`, `scanning`, `connecting`, `connected`,
`auth retry`, `waiting for network`, `retry`, `setup AP`). `network_reason`
says why the device is not connected, in the same words as the display card:
`not_found` (the saved network was not seen; rescanning), `password`
(authentication failed; retrying), `setup_ap` (only the setup network runs:
no credentials or AP fallback), `connecting` (scanning, connecting or retry
backoff) and an empty string while connected.

Malformed JSON includes `json_offset` when the common grammar identifies the
failure: a zero-based byte position in the original UTF8 request, or its byte
length for unexpected EOF. It counts bytes, not decoded characters, and never
echoes request values. Typed field violations continue to identify `field`.
The Web error includes the position without changing or resending the request.

`storage_full` (HTTP507) means exhausted image IDs, image-byte quota,
replacement workspace or actual ENOSPC from a checked file operation. `memory`
(HTTP507) means an allocation/ENOMEM failure; other file I/O faults remain
`internal` (HTTP500). Quotas/publication are unchanged. Failed write/sync/close/
rename never publishes a candidate; old active image/names data remains.
JPEG validation preserves its first stdio error through checked close, while
an expired absolute deadline still takes precedence as timeout. Partial-delete
receipts report committed reference cleanup and remaining files separately.

## Error causes and codes

Every competition row and every selection reports why it failed as a stable
cause key, and a short error code for the owner to quote: provider
abbreviation (`OLG` OpenLigaDB, `APF` API-Football, `ESPN`, `FDO`
football-data.org), `E` and the cause number, and the HTTP status after "·"
when the provider answered with one, e.g. `OLG E10·503`. The display shows
the code without the HTTP status. Numbers are assigned once and never
change. The browser translates the key into the cause and the action; the
firmware has no text of its own beyond the display's short text. A successful
request clears only its dataset's request failure. Another dataset's failure,
an unmaintained match or a capacity notice retains its own clearing condition.

| No. | Key | Meaning |
|---|---|---|
| E01 | `provider_off` | The provider needs a key or opt-in that is not set |
| E02 | `budget_day` | The provider's or the global daily request budget is used |
| E03 | `busy` | Another heavy operation held the network too long |
| E04 | `net_lost` | Network lost or changed during the request |
| E05 | `time_invalid` | No valid time |
| E06 | `settings_changed` | Settings changed during the request |
| E07 | `rate_limited` | HTTP 429: the provider paused the device |
| E08 | `auth` | HTTP 401 or 403: key wrong or not allowed |
| E09 | `not_found` | HTTP 404: competition, season or club unknown to the provider |
| E10 | `http_error` | Any other HTTP status than 200 |
| E11 | `connect` | No HTTP response: DNS, TCP or TLS failed |
| E12 | `bad_response` | Response missing, incomplete or unreadable |
| E13 | `too_large` | Response or result exceeds the device's capacity |
| E14 | `memory` | Out of memory |
| E15 | `other` | Any other failure |
| E16 | `unmaintained` | The provider leaves past matches without a result (more than 24 h) |
| E17 | `no_season` | The provider reports no current season |
| E18 | `counter` | The daily request counter could not be stored |
| E19 | `store_full` | Not all data of the competition fit the device's store |

`GET /wifi` reports its own `error_code` keys (above); the browser shows the
key itself as the code. Failed requests answer with a `code` of their own
("Errors" below), shown with the HTTP status of the response.

## Sessions and request validation

The admin password is optional. Mutations nevertheless require a random
128-bit session token (header `X-CSRF-Token`) and same-origin/Host validation,
including login, to reduce unintended browser requests. A configured password
is checked before the token is issued. There are four session slots
(`cfg::kSessionSlots`); a fifth login replaces the least recently used one, so
two browsers can work in parallel. A session expires 15 minutes after its last
authenticated request (sliding, `cfg::kSessionLifetimeMs`); every request that
carries the token, including every read endpoint, moves the expiry (one
common boundary in front of all read routes; a read without a token or with
an unknown one is answered unchanged and opens no session). Saving settings keeps all
sessions; only changing the admin password ends every other session. `POST
/logout` ends the caller's session. `GET /session` tells the page whether a
password is set and whether its stored token is still valid.

The Host header must name this device (its STA or AP address, `<hostname>.local`
or the bare hostname, compared case-insensitively; `:80` is accepted) and an
`Origin` header, if sent, must be `http://` plus that host. GET settings/status
do not return passwords or API keys; explicit export is a protected mutation.
Application Apply and small typed commands reject unknown and duplicate keys,
invalid scalar types, malformed Unicode and trailing non-space bytes. Label
schemas reject embedded NUL; raw SSID bytes are intentionally separate. The
same JSON reader/encoder handles settings, screen rules, names and commands;
there is no body-sized JSON allocation. API keys are not passed into
the debug producer.

The debug WebSocket takes its token from the URL query because the browser
WebSocket API cannot set headers. The log line of a rejected request omits the
query, and the stream is closed as soon as its session ends or Browser debug
is switched off. The server checks the origin, the Browser debug setting, the
session token and the single free socket slot before it answers the handshake
(a pre-handshake callback; `CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`).
A refused request gets an ordinary HTTP error with the usual body and no
upgrade: 403 `origin`, 403 `debug_disabled`, 401 `session` or 409 `busy`.
The device only sends frames; frames from the browser are read and discarded,
and the slot is freed when the TCP connection ends.

Each diagnostic frame uses one pinned bounded buffer and queued HTTP-task
work. Before writing, the server rechecks the original authenticated session,
connection detachment and the socket's actual WebSocket state. Logout, expiry
or disabling debug prevents another frame. Closing and reconnecting cannot
reuse an older queued frame, including with the same token and socket number;
stale work does not clear or close a newer stream or a public HTTP connection.
Every partial frame-header/payload write must complete within its socket
window. Failed queue admission releases the buffer for a later sample.

## Errors

Before a response body starts, a failed request answers `{"error": "<English text>", "code": "<stable
code>"[, "field": "<name>"]}` with `Cache-Control: no-store`. `code` is for
programs, `error` for people, `field` names the one value that caused a 400:
the dotted settings path (for example `competitions[0].primary.competition`),
a body key (`competition`, `season`, `id`, `role`, `action`, `confirm`) or a header
(`X-Image-Id`, `X-Image-Role`). For value errors `code` is the settings reason (`invalid`,
`range`, `type`, `unknown_key`, `too_long`, `too_many`, `required`,
`duplicate`, `conflict`, `none_enabled`). Other codes: `origin`
(403), `session`, `password` (401), `trial`, `busy`, `state`, `changed`, `settings_changed`, `demo_disabled` (409),
`unknown_route`, `not_found` (404), `method` (405), `body_missing`,
`body_incomplete`, `json`, `size`, `content`, `invalid` (400),
`body_too_large` (413), `content_type` (415), `timeout` (408), `memory`,
`storage_full` (507), `screenshot_unavailable` (503), `internal` (500).
Once a binary stream has started, transfer failures abort the connection
instead of adding an incompatible JSON body.

All ordinary response fragments share the 30-second request deadline through
every underlying partial socket send; positive partial progress cannot extend
that deadline. Screenshot capture keeps its separate five-second C6 cap.
JSON command media types must be exactly `application/json`, case-insensitively,
with optional surrounding spaces/tabs and semicolon parameters. Prefixes such
as `application/jsongarbage` are rejected with415 `content_type`.

"Every 400 names its field" applies to value errors: a settings value, a
body key or a header that is present but wrong. Errors about the request as a
whole (`body_missing`, `body_incomplete`, `json`, `size`, `content`) have no
single field and carry only `code`.

The unread rest of a request body is read and discarded before the answer, up
to `cfg::kDrainMaxBytes` (32 KB), so the browser reads the error instead of a
connection reset and the connection stays usable. A body above that limit
(firmware or an oversize image) cannot be drained; the connection is closed
after the answer and a browser may report a network error instead of the
message. The Web UI checks the size before uploading for this reason.

### Visible names read status

`GET /status` includes `names:{pending,error,code}` for the overview's visible
name lookup. Heavy-operation contention is pending with an empty code; a real
read/close/reference failure returns `NAM E01`. Provider fallback remains
visible and the Web UI gives collection/backup recovery advice. A failed lookup
does not publish a complete cache that could hide the failure on the next poll.
This adds only a4-B local error result and streamed fields, no resident model,
heap allocation or stored setting.
