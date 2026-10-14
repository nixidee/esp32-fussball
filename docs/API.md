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
| GET `/api/v1/competitions` | The competition store: competitions with their ids and fixture pairs |
| GET `/api/v1/fixtures?competition=ID&source=primary\|fallback` | The club's matches of a competition (by its id) for the pair picker |
| GET `/api/v1/teams?competition=ID` | The teams of a competition in the shown data with the owner's short names |
| GET `/api/v1/images`, `/images/file?id=N\|stock=crest\|background[&r=rev]` | Image inventory: library images with per-image revision, and the two roles `crest` and `background` with `replaced`, replacement `bytes` and `revision` (it changes when the role's image changes); the JPEG itself (`stock=` delivers the role's replacement, else its factory image) |
| POST `/api/v1/session`, `/logout` | Open/close the mutation session |
| POST or PUT `/api/v1/settings` | Strictly typed settings patch |
| POST or PUT `/api/v1/competitions` | Replace the competition list and/or the fixture pairs |
| POST `/api/v1/teams` | Set or remove the owner's short name of one team |
| PUT `/api/v1/teams` | Replace all of the owner's short names (backup import) |
| POST `/api/v1/export` | Explicit export of the settings, the competition store and the short names |
| POST `/api/v1/wifi/scan`, `/data/select`, `/data/fixtures`, `/data/refresh` | Start asynchronous scan, provider list, fallback fixtures or refresh |
| POST `/api/v1/display` | Quick display action `{"action":"next"\|"ip"}` (same events as the buttons) |
| POST `/api/v1/images/upload`, `/images/delete` | Upload a library image (optional `X-Image-Id` replaces one; `X-Image-Role: crest\|background` uploads the replacement of that role instead, answer `{"ok":true,"role":…}`) or delete a library image by id |
| POST `/api/v1/images/role` | `{"role":"crest"\|"background","action":"reset"}` deletes the replacement, so the factory image shows again; `{"role":…,"action":"copy","id":N}` copies library image N as the replacement (the library image stays). Roles cannot be deleted. A replacement counts towards the image budget, not towards the ten library ids |
| POST `/api/v1/images/repair` | Confirmed image-storage formatting and reference cleanup; preserves other settings and does not restart |
| POST `/api/v1/ota` | Stream compatible firmware to the inactive slot |
| POST `/api/v1/reboot`, `/reset` | Reboot or reset settings (also deletes the kept football data and the short names), optionally delete all images or format image storage |
| WebSocket `/api/v1/debug?token=` | Controlled live status while the Debug tab is open; ends with the session that opened it |

After the first full path, shortened paths in the table retain `/api/v1`.
See [WEB_UI.md](WEB_UI.md) for the browser workflow and image contract.

## Endpoint details

`GET /wifi` returns `busy` for a queued or running user scan and an
`error_code`: empty after success, `scan_start`, `scan_result`, `net_lost` or
`timeout` on failure. The list and state are read together. Connection scans
have priority; a user scan runs with the existing 15-second scan deadline.
Duplicate user requests return 409 `busy`; a failed or cancelled job is never
represented as a successful empty scan.

The schema publishes `ota_max` from the actual inactive OTA slot and
`selection_text_max` from the shared search/country capacity. `firmware_id`
in status is the full 64-character lowercase hexadecimal ELF SHA256 from the
app descriptor. It distinguishes builds with equal version strings; it is
not the shortened hash printed by the panic handler.
The schema lists the screens in display order as `screen_order` (screen
indices as in [CONFIGURATION.md](CONFIGURATION.md): 0 Own match, 1
Conference, 2 Table, 3 Crest/slideshow, 4 Matchday) and the keys of a
competition's `screens` switches as `competition_screens`. Settings carry
`table_page_s` (table step, bounds in the schema). Status `screen` is the
index of the shown screen, 0–4.
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

The competitions and the fixture pairs live in their own store with its own
record (see [CONFIGURATION.md](CONFIGURATION.md)); the settings JSON does not
carry them. `GET /competitions` returns

```json
{"competitions":[{"id":1,"active":true,"fallback_active":false,
  "primary":{"provider":0,"competition":"bl1","season":"2025","team":"40"},
  "fallback":{"provider":0,"competition":"","season":"","team":""},
  "competition_name":"1. Bundesliga","team_name":"FC Bayern",
  "screens":{"own_match":true,"conference":true,"matchday":true,"table":true}}],
 "fixture_pairs":[{"active":true,"swapped":false,"competition":1,
  "primary_id":"…","secondary_id":"…"}]}
```

`provider` is the index into the schema's `providers` list (0 `openligadb`,
1 `api-football`, 2 `espn`, 3 `football-data`). Every competition has a stable id from 1 to 255. The device assigns it when
an entry is created and never changes it while the entry exists; the
position in the list only sets the display order. `screens` says on which
screens the competition appears. A fixture pair names its competition by id.
At most `limits.competitions` competitions (8) and `limits.fixture_pairs`
pairs exist (schema).

`POST` or `PUT /competitions` takes `{"competitions":[…]}`,
`{"fixture_pairs":[…]}` or both. A list in the body replaces that whole
list; a list that is absent stays. In an entry, an absent key takes the value
of a new entry, and an absent `id` or `id` 0 creates a new entry that the
device gives the next free id; any other id is kept as given (a restored
backup keeps its ids) and must be unique in the list. The store is validated as a whole (unique
ids, pairs that reference an existing competition with a club, usable
providers) and saved in one step. The answer is `{"ok":true,
"competitions":[…], "fixture_pairs":[…]}` with the assigned ids. A rejected
body answers 400 with the field path, for example
`competitions[2].primary.competition` or `fixture_pairs[0].competition`
(code `reference` for an unknown id).

`GET /fixtures?competition=ID` and `POST /data/fixtures {"competition":ID}`
take the competition id, not a list position; an id that does not exist
answers 400 `reference`.

## Status fields

`GET /status` reports the numbers needed to judge memory under load without a
serial port: `heap_free`, `heap_largest` and `heap_min` (lowest free heap since
boot, internal 8-bit memory) plus `tasks`, an array of `{name, id,
stack_min_free}` with the lowest free stack in bytes of every task (at most 16).

`settings_source` says where the current settings come from: `stored` (the
stored record or the last save), `initial` (no record, or after a reset),
`rejected` (the stored record has a value outside its limits), `damaged`
(too long or not decodable), `unreadable` (NVS read error or no memory) or
`no_storage` (NVS unusable). Every value except `stored` means the initial
values apply. A `rejected` or `damaged` record is overwritten by the next
save; `unreadable` and `no_storage` refuse saving until a reset.
`competitions_source` reports the competition store in the same words
(`stored` also after a one-time migration of the competitions from an older
settings record).

`competitions` has one row per competition in list order: `{id, matches,
provider, error}`. `own_match.competition` is the id of the competition of
the club's current or next match.

`last_update_ms` is the monotonic time of the oldest last successful
provider request over the active competitions (0 = none yet). Data kept
from before a reboot carry their age, so the value can be negative. `data_stale`
is true while a competition's primary source failed and no fallback source
stands in, or while a shown match is in its live window and the last
successful match request of its competition is older than the stale limit.
`competitions[i].error` is true for every competition whose primary source
failed without a fallback standing in; it keeps its earlier data until a
later request succeeds, while the other competitions update. `data_error`
and `data_error_code` explain the first such competition. `matches_truncated`
is true when not every match of a competition fitted the match store; it
then holds the matches nearest in time and every tracked-club match that
fits.

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

## Provider-operation error keys

Status pairs `data_error` with `data_error_code`; selection/fallback fixture
responses pair `error` with `error_code`. The text remains diagnostic, while
the browser translates the stable key. The cause is reset at each operation
boundary and successful results clear previous errors. `last_update_ms` is
the oldest age over the competitions; competition rows report source, match
count and an error flag, not their own fetch age. Warnings derive from existing state fields,
without a second warnings array.

| Key | Meaning |
|---|---|
| empty | No error |
| `not_started`, `provider_off` | Service not started or provider unavailable by key/opt-in |
| `budget_day`, `busy` | The provider's daily cap is reached, or another heavy operation prevented admission |
| `net_lost`, `time_invalid`, `settings_changed` | Network, clock or settings invalidated the operation |
| `rate_limited`, `auth`, `not_found`, `http_error` | HTTP 429, 401/403, 404 or another non-200 response |
| `connect` | No HTTP response; connection failed before a response |
| `bad_response`, `too_large`, `memory` | Unreadable/incomplete result, exceeded capacity or allocation failure |
| `other` | An unclassified failure; inspect the diagnostic text/log |

## Sessions and request validation

The admin password is optional. Mutations nevertheless require a random
128-bit session token (header `X-CSRF-Token`) and same-origin/Host validation,
including login, to reduce unintended browser requests. A configured password
is checked before the token is issued. There are four session slots
(`cfg::kSessionSlots`); a fifth login replaces the least recently used one, so
two browsers can work in parallel. A session expires 15 minutes after its last
authenticated request (sliding, `cfg::kSessionLifetimeMs`); every request that
carries the token, including reads, moves the expiry. Saving settings keeps all
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
`duplicate`, `reference`, `conflict`, `none_enabled`). Other codes: `origin`
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
