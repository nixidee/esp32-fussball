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
| GET `/api/v1/fixtures?competition=N&source=primary\|fallback` | The club's matches of a competition for the pair picker |
| GET `/api/v1/images`, `/images/file?id=N\|stock=crest\|background[&r=rev]` | Image inventory: library images with per-image revision, and the two roles `crest` and `background` with `replaced`, replacement `bytes` and `revision` (it changes when the role's image changes); the JPEG itself (`stock=` delivers the role's replacement, else its factory image) |
| POST `/api/v1/session`, `/logout` | Open/close the mutation session |
| POST or PUT `/api/v1/settings` | Strictly typed settings patch |
| POST `/api/v1/export` | Explicit settings export |
| POST `/api/v1/wifi/scan`, `/data/select`, `/data/fixtures`, `/data/refresh` | Start asynchronous scan, provider list, fallback fixtures or refresh |
| POST `/api/v1/display` | Quick display action `{"action":"next"\|"ip"}` (same events as the buttons) |
| POST `/api/v1/images/upload`, `/images/delete` | Upload a library image (optional `X-Image-Id` replaces one; `X-Image-Role: crest\|background` uploads the replacement of that role instead, answer `{"ok":true,"role":…}`) or delete a library image by id |
| POST `/api/v1/images/role` | `{"role":"crest"\|"background","action":"reset"}` deletes the replacement, so the factory image shows again; `{"role":…,"action":"copy","id":N}` copies library image N as the replacement (the library image stays). Roles cannot be deleted. A replacement counts towards the image budget, not towards the ten library ids |
| POST `/api/v1/images/repair` | Confirmed image-storage formatting and reference cleanup; preserves other settings and does not restart |
| POST `/api/v1/ota` | Stream compatible firmware to the inactive slot |
| POST `/api/v1/reboot`, `/reset` | Reboot or reset settings, optionally delete all images or format image storage |
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
settings or restarts. USB `uploadfs` restores stock files.

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

`last_update_ms` is the monotonic time of the oldest data shown and
`data_stale` is true while any competition shows earlier data.
`competitions[i].error` is true for every competition whose last fetch
failed; it keeps its earlier data until a later fetch succeeds, while the
other competitions update. `data_error` and `data_error_code` explain the
first failed competition. `matches_truncated` is true when not every match
fitted the shared match list; the published list then holds the matches
nearest in time and every tracked-club match that fits.

`next_request_ms` is the monotonic time the next provider round is due
(compare with `uptime_s`; 0 = none scheduled) and `next_request_reason` says
why it is due then: `live` (a match in its window is running, 60 s),
`prematch` (a match window is open before kickoff, 5 minutes), `postmatch`
(a match in its window has finished, 5 minutes), `idle` (no match window,
6 hours), `window` (the next match window opens earlier), `budget` (a daily
cap stretches the interval), `budget_used` (a daily cap is used up; retried
every 5 minutes, requests are only sent after 00:00 UTC), `retry` (a failed
competition or round is retried), `waiting` (no network or no valid time;
`next_request_ms` 0), `update_test` (paused while a new firmware is in its
update test; `next_request_ms` 0) and `none` (nothing scheduled yet after a
start). A refresh from the Web UI starts earlier than `next_request_ms`.

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
boundary and successful results clear previous errors. Snapshot age is global;
competition rows report source, match count and an error flag, not an
independently measured fetch age. Warnings derive from existing state fields,
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
