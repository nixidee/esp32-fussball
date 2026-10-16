# Device REST API

The device's own HTTP API under `/api/v1`, used by the Web UI and usable by
other local programs. It is not a football data provider; the providers are
described in [DATA_PROVIDERS.md](DATA_PROVIDERS.md). The web server, its
admission and operation bounds and local-HTTP security are described in
[NETWORK.md](NETWORK.md); the browser workflow in [WEB_UI.md](WEB_UI.md).
Implementation: `components/web/web_service.cpp`,
`components/web/settings_json.cpp`.

## Endpoints

| Method and endpoint | Purpose |
|---|---|
| GET `/api/v1/status`, `/schema`, `/settings` | Status (screen, own match, competition state, full build identity, heap/task minima), bounds/options and upload limits, redacted settings |
| GET `/api/v1/session` | `{password_set, valid}` for the token the request carries |
| GET `/api/v1/wifi`, `/selection` | Bounded scan/selection results |
| GET `/api/v1/competitions` | The competition store: competitions with their ids |
| GET `/api/v1/teams?competition=ID` | The teams of a competition in the shown data with the owner's short names |
| GET `/api/v1/images`, `/images/file?id=N\|stock=crest\|background[&r=rev]` | Image inventory: library images with per-image revision, and the two roles `crest` and `background` with `replaced`, replacement `bytes` and `revision` (it changes when the role's image changes); the JPEG itself (`stock=` delivers the role's replacement, else its factory image) |
| POST `/api/v1/session`, `/logout` | Open/close the mutation session |
| POST or PUT `/api/v1/settings` | Strictly typed settings patch |
| POST or PUT `/api/v1/competitions` | Replace the competition list |
| POST `/api/v1/teams` | Set or remove the owner's short name of one team |
| PUT `/api/v1/teams` | Replace all of the owner's short names (backup import) |
| POST `/api/v1/export` | Explicit export of the settings, the competition store and the short names |
| POST `/api/v1/wifi/scan`, `/data/select`, `/data/refresh` | Start asynchronous scan, provider list or refresh |
| POST `/api/v1/display` | Quick display action `{"action":"next"\|"ip"}` (same events as the buttons) |
| POST `/api/v1/images/upload`, `/images/delete` | Upload a library image (optional `X-Image-Id` replaces or restores that valid library id; `X-Image-Role: crest\|background` uploads the replacement of that role instead, answer `{"ok":true,"role":…}`) or delete a library image by id |
| POST `/api/v1/images/role` | `{"role":"crest"\|"background","action":"reset"}` deletes the replacement, so the factory image shows again; `{"role":…,"action":"copy","id":N}` copies library image N as the replacement (the library image stays). Roles cannot be deleted. A replacement counts towards the image budget, not towards the ten library ids |
| POST `/api/v1/images/repair` | Confirmed image-storage formatting and reference cleanup; preserves other settings and does not restart |
| POST `/api/v1/ota` | Stream compatible firmware to the inactive slot |
| POST `/api/v1/reboot`, `/reset` | Reboot or reset settings (also deletes the kept football data and the short names), optionally delete all images or format image storage |
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

`POST /display` keeps its existing `next` and `ip` bodies. Commands are
queued with the current settings version internally; callers supply no new
field. A successful settings/competition save discards earlier commands,
including delayed delivery, while preserving commands issued for the new
version. Physical gestures already in progress are cancelled too. A full
action queue still returns 409 `busy`.

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
WiFi. After recovery the active error clears. The existing ten-second OTA
trial still rejects an initial startup failure even if it later recovers.

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

`GET /images/file?display=1` serves the currently selected actual file,
including its factory fallback while a damaged replacement still exists.
It returns the existing missing-file error when no candidate is selected.
This dynamic response always uses `no-cache`; an optional `r` cache-buster
does not make it immutable. Existing `id=N` and `stock=crest|background`
downloads retain their original raw-library/role choice and revisioned
immutable caching. A read error aborts the chunk stream without a normal
completion marker, rather than reporting a truncated JPEG as complete.

`GET /wifi` returns `busy` for a queued or running user scan and an
`error_code`: empty after success, `scan_start`, `scan_result`, `net_lost` or
`timeout` on failure. The list and state are read together. Connection scans
have priority; a user scan runs with the existing 15-second scan deadline.
Duplicate user requests return 409 `busy`; a failed or cancelled job is never
represented as a successful empty scan.

`GET /selection` returns the owner's competition or club list started by
`POST /data/select`: `busy` while it runs, then `items` (`id`, `name`,
`season`; at most one page), `more`, `revision` and `error_code` (a cause
key below; empty after success). After a failure `code` holds the short
error code and `http` the HTTP status the provider answered with, if any. While `busy`, `wait` says what holds the
next request of the list and `wait_s` for how many more seconds: `limit`
(the provider's or the global window limit) or `retry` (the provider
answered "too many requests" and is paused); `none` with `wait_s` 0 while
the request runs or only waits for the 2-second device gap. A waiting list
never blocks the device: the jobs of other providers run meanwhile.

The schema publishes `ota_max` from the actual inactive OTA slot and
`selection_text_max` from the shared search/country capacity, and the
short-name bounds of `PUT /teams`: `short_names_max` (entries over all
competitions) and `short_name_max` (bytes per name). `firmware_id`
in status is the full 64-character lowercase hexadecimal ELF SHA256 from the
app descriptor. It distinguishes builds with equal version strings; it is
not the shortened hash printed by the panic handler.
The schema lists the screens in display order as `screen_order` (screen
indices as in [CONFIGURATION.md](CONFIGURATION.md): 0 Own match, 1
Conference, 2 Table, 3 Crest/slideshow, 4 Matchday) and the keys of a
competition's `screens` switches as `competition_screens`. Settings carry
`table_page_s` (table step, bounds in the schema) and `matchday_mode`
(matchday the Matchday screen shows: 0 current, 1 next). Status `screen` is the
index of the shown screen, 0–4. `screen_starting` reports whether the last
display poll still showed its initial three-step startup card. After a
network initialization fault ends that card, the browser preview uses this
existing display state rather than restarting it from connection guesses.
Status `requests` contains each provider's request attempts since boot,
including selection requests. It is not a daily usage or remaining-credit
counter and must not be compared directly with the configured daily caps.

Image repair requires `{"confirm":"repair"}`, same-origin/session admission,
the heavy-operation token and no OTA trial. It formats LittleFS (including
stock files), then saves cleaned image references only if necessary. Success
returns `{"ok":true,"storage_formatted":true}`. A failed reference save after
formatting returns HTTP 500, `code:"repair_references"` and
`storage_formatted:true`; the filesystem operation already happened and must
not be mistaken for a fully successful repair. Neither result resets other
settings or restarts. USB `uploadfs` restores stock files. The football
service writes its kept competition data and the short names again after a
repair.

Status `fs_total` and `fs_used` cover the whole LittleFS partition: images,
stock files, the kept football data and the short names.

## Short names

`GET /teams?competition=ID` lists every team of the competition's matches
and table rows that the device holds now (all providers):

```json
{"competition":1,"max":48,"items":[
  {"provider":0,"id":40,"short_name":"Bayern","custom":"FCB"}]}
```

`provider` is the provider index, `id` the provider's team id,
`short_name` the provider's name and `custom` the owner's replacement
(`""` = none). `max` is the number of replacements the device keeps over
all competitions. A competition without data returns an empty list; a
publication during the read returns 409 `changed`.

`POST /teams` with `{"provider":P,"id":N,"name":"FCB"}` needs the mutation
session. `name` has at most 26 bytes UTF-8 and no control characters; `""`
removes the replacement. The change is written to the device at once and
shows on the display with the next redraw. Errors: 400 for a rejected field,
409 `busy` while another heavy operation runs, 409 `state` without a
mounted filesystem or during the first boot of an update, 507
`storage_full` when the table is full or the file does not fit (the
previous name stays).

`PUT /teams` with `{"short_names":[{"provider":P,"id":N,"name":"FCB"}]}`
replaces every replacement at once (the import of a backup; `[]` removes
all) and writes the file once. It needs the mutation session; at most
`max` entries, each name 1 to 26 bytes UTF-8 without control characters,
each team once. A rejected entry returns 400 with its path, for example
`short_names[3].name`; the other errors are those of `POST /teams`, and
on any error the previous names stay. `POST /export` returns the same
`short_names` array next to the settings and the competition store.

## Competitions

The competitions live in their own store with its own
record (see [CONFIGURATION.md](CONFIGURATION.md)); the settings JSON does not
carry them. `GET /competitions` returns

```json
{"competitions":[{"id":1,"active":true,
  "primary":{"provider":0,"competition":"bl1","season":"2025","team":"40"},
  "competition_name":"1. Bundesliga","team_name":"FC Bayern",
  "screens":{"own_match":true,"conference":true,"matchday":true,"table":true}}]}
```

`provider` is the index into the schema's `providers` list (0 `openligadb`,
1 `api-football`, 2 `espn`, 3 `football-data`). Every competition has a stable id from 1 to 255. The device assigns it when
an entry is created and never changes it while the entry exists; the
position in the list only sets the display order. `screens` says on which
screens the competition appears. `primary` is the competition's only source
(provider, competition, season, club; the key keeps its name for
compatibility). At most `limits.competitions` competitions (8) exist
(schema).

`POST` or `PUT /competitions` takes `{"competitions":[…]}`. The list in the
body replaces the whole list; without it the list stays. Any other key,
including `fixture_pairs`, `fallback` or `fallback_active` of earlier
firmware, is rejected as unknown. In an entry, an absent key takes the value
of a new entry, and an absent `id` or `id` 0 creates a new entry that the
device gives the next free id; any other id is kept as given (a restored
backup keeps its ids) and must be unique in the list. The store is validated as a whole (unique
ids, usable providers) and saved in one step. The answer is `{"ok":true,
"competitions":[…]}` with the assigned ids. A rejected body answers 400 with
the field path, for example `competitions[2].primary.competition`.

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

`settings_source` says where the current settings come from: `stored` (the
stored record or the last save), `initial` (no record, or after a reset),
`outdated` (the stored record has an older format that this firmware no
longer reads), `rejected` (the stored record has a value outside its limits), `damaged`
(too long or not decodable), `unreadable` (NVS read error or no memory) or
`no_storage` (NVS unusable). Every value except `stored` means the initial
values apply. An `outdated`, `rejected` or `damaged` record is overwritten by
the next save; `unreadable` and `no_storage` refuse saving until a reset.
`competitions_source` reports the competition store in the same words; it is
also `outdated` when no competition record exists and the settings record is
outdated (older firmware kept the competitions in the settings record).

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
provider request over the active competitions (0 = none yet). Data kept
from before a reboot carry their age, so the value can be negative. `data_stale`
is true while the last request of an active competition failed, or while a shown match is in its live window and the last
successful match request of its competition is older than the stale limit.
A competition whose last request failed has a `cause`; it keeps its earlier
data until a later request succeeds, while the other competitions update.
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

## Error causes and codes

Every competition row and every selection reports why it failed as a stable
cause key, and a short error code for the owner to quote: provider
abbreviation (`OLG` OpenLigaDB, `APF` API-Football, `ESPN`, `FDO`
football-data.org), `E` and the cause number, and the HTTP status after "·"
when the provider answered with one, e.g. `OLG E10·503`. The display shows
the code without the HTTP status. Numbers are assigned once and never
change. The browser translates the key into the cause and the action; the
firmware has no text of its own beyond the display's short text. A
successful request clears the cause.

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
Unknown keys, wrong types, excess arrays, embedded NULs and trailing non-space
JSON bytes are rejected. API keys are not passed into the debug producer.

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

## Errors

Every failed request answers `{"error": "<English text>", "code": "<stable
code>"[, "field": "<name>"]}` with `Cache-Control: no-store`. `code` is for
programs, `error` for people, `field` names the one value that caused a 400:
the dotted settings path (for example `competitions[0].primary.competition`),
a body key (`competition`, `season`, `id`, `role`, `action`, `confirm`) or a header
(`X-Image-Id`, `X-Image-Role`). For value errors `code` is the settings reason (`invalid`,
`range`, `type`, `unknown_key`, `too_long`, `too_many`, `required`,
`duplicate`, `conflict`, `none_enabled`). Other codes: `origin`
(403), `session`, `password` (401), `trial`, `busy`, `state`, `changed` (409),
`unknown_route`, `not_found` (404), `method` (405), `body_missing`,
`body_incomplete`, `json`, `size`, `content`, `invalid` (400),
`body_too_large` (413), `content_type` (415), `timeout` (408), `memory`,
`storage_full` (507), `internal` (500).

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
