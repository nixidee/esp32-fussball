# Network — WiFi, setup AP, Web server and OTA

The C6 implementation is compiled in the current development candidate. Network,
browser, timeout, RF and rollback acceptance have not been run for this candidate.
Defaults and bounds live in `include/app_config.h`; this document explains the
implemented contract.

## WiFi connection manager

`network::WifiPolicy` owns the transition rules; `network_service.cpp` applies
them through ESP-IDF. Success means `IP_EVENT_STA_GOT_IP`, not association.

| State | Behaviour |
|---|---|
| AP only | No credentials: start the setup AP |
| Scanning | Targeted scan for the saved SSID, bounded to 15 seconds |
| Connecting | Association and DHCP, bounded to 30 seconds |
| Connected | Publish the IP and stop the setup AP immediately |
| Authentication backoff | Retry after 10, 30 and 60 seconds; then AP fallback |
| SSID wait | Rescan every 20 seconds; AP fallback after five minutes |
| AP fallback | Keep setup available and retry authentication every five minutes |

Other disconnects use bounded exponential backoff and jitter. A lost connection
first triggers reconnect; a short interruption does not immediately open the AP.
An AP client pauses background scans because scanning can move the radio away
from the AP channel. Existing scan cancellation and stale completion events are
handled before applying a new credential selection.

One network is stored. SSIDs are copied by length, including valid 32-byte SSIDs.
Passwords are empty, 8–63 bytes, or exactly 64 hexadecimal characters. Enterprise
authentication is not implemented. Only credential, AP-password and antenna
changes restart radio configuration; changing a screen or club does not.

The C6 hardware profile configures GPIO3/GPIO14 once at network start and
selects them before RF start and on antenna changes, including the vendor's
100 ms settling interval. See
[HARDWARE.md](HARDWARE.md). Actual internal/external antenna reception remains
unverified.

Station modem sleep is off on every profile (`kWifiPowerSave` in
`app_config.h`; the ESP-IDF default would be `WIFI_PS_MIN_MODEM`). The device
is mains powered, and in modem sleep the access point holds incoming frames
until the next beacon, which delays every TCP round trip. Its effect on
provider transfer time still needs a device log.

The C6 SDK configuration enables IPv4 and disables unused IPv6, enterprise WiFi
and WiFi IRAM speed optimisations. This reduces fixed RAM use while retaining
ordinary STA/AP operation. Throughput and reconnect costs need device acceptance.

## Setup AP, DNS and mDNS

The SSID is `Fussball-` followed by six hexadecimal MAC suffix characters.
The AP password is optional; an empty password opens the AP, while a configured
valid passphrase enables WPA2. `cfg::kApIp` defines the AP address, currently
`192.168.4.1`; the gateway uses the same address with a `/24` subnet.

Before setting this address, initialization explicitly stops the AP DHCP
server. A freshly created ESP-IDF interface is in `DHCP_INIT`, which is not
the `DHCP_STOPPED` state required by `esp_netif_set_ip_info`. Initialization
then re-arms DHCP while the interface is down; the first AP-start event starts
address allocation for clients. Every step checks its result. Omitting the
stop causes `ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED` before WiFi initialization,
independent of whether the configured station SSID is available.

The device's `Kein WLAN` / `No WiFi` status means that the connection manager
has not published a station IP lease. It does not distinguish scanning,
authentication or initialization errors; the serial boot log reports a
startup error as `boot: network init: <error>`.

A bounded UDP DNS responder directs captive-portal lookups to the AP address.
Its socket is opened before its static 2 KB task starts; startup failure is
reported. The full Web UI and firmware upload are available in AP mode.
Automatic captive-portal opening depends on the phone/browser and remains to be
checked. The setup AP stops immediately on a STA IP lease.

The managed mDNS component is pinned to `espressif/mdns@1.14.0`, checked against
the component registry on 2026-10-10. The default host is `fussball`, giving
`http://fussball.local`. The saved hostname overrides the initial preset.
The mDNS worker has a 4 KB SDK-configured stack.

## Time

SNTP starts after STA obtains an IP and uses the stored server, default
`pool.ntp.org`. Its synchronisation hook passes received UTC to the existing
time service. The time service remains the only owner of clock validity,
`settimeofday()`, source tracking and time-change notifications.

The saved location label selects one of 40 compiled POSIX time-zone rules.
The standard Picolibc `tzset()` and `localtime_r()` calls run behind the time
service mutex. Rules derive from IANA tzdata 2026c footers and were compared
with 2026e for 2026–2028. This is a recurring-rule set, not a historical time-zone
database. Fixture instants remain UTC; local time is used for display and night
mode. Clock validity starts false on every boot.

All retries, deadlines, freshness and overlay durations use the monotonic ESP
timer. A UTC correction cannot move these timers. Night windows include their
start and exclude their end; a window crossing midnight is supported, equal
endpoints mean an empty window, and daylight-saving changes follow local time.

The earlier offline C6 time test passed on 2026-10-09: 112 rule cases, 12 actual
night-window cases, backward/forward clock jumps and event/validity checks.
That evidence covers the core time service. SNTP reception, network-origin
corrections and reconnect behaviour in this candidate remain unaccepted.

## Admission and operation bounds

One shared heavy-operation guard admits provider TLS/JSON work, image
validation/publication/deletion and OTA. Competing heavy operations return
HTTP 409 or wait for a later scheduled provider attempt. Normal screen redraw,
including its bounded JPEG decoder, is still allowed during provider traffic;
that overlap must be measured.

A provider operation has a 30-second monotonic deadline, one-second socket
wait slices, an 8-second limit for provider silence while sending a request or
receiving its response, no automatic redirects, an 8 KB incoming-header cap and
body/wire caps described in [DATA_PROVIDERS.md](DATA_PROVIDERS.md). DNS runs
asynchronously in the existing lwIP thread with one fixed, token-protected
hostname/result record. Late callbacks cannot reference a dead caller or
overwrite a newer job. TLS uses incremental nonblocking connection/handshake
calls; the socket stays nonblocking afterwards, so reads and writes wait in
slices for a complete TLS record or send space. The original host is retained
for certificate verification and SNI after numeric IPv4 resolution.
Cancellation checks run between steps for time, settings generation and network
epoch. SDK scheduling and cryptographic step duration still require measurement;
the implementation does not claim a measured hard real-time bound.

Every provider request writes one serial log line (tag `provider`): either
`not sent: <URL> (<reason>)` when it stops before the connection (provider not
enabled, settings changed, network lost or changed, operation busy, daily
budget used or budget unavailable), or `<URL>: HTTP <status>, <bytes> B,
<duration> ms[, <error>]` after the transfer. API keys travel in request
headers and never appear in the URL. Every rejected Web request writes one
line (tag `web`) with method, path without query, HTTP status and reason, which
explains the HTTP server's generic `uri handler execution failed` warning.

The HTTP server has a 10 KB task stack, four client slots and bounded request
headers. Each session's receive override starts a 30-second request deadline
with the first header bytes; body reads and response writes share that deadline.
Idle socket wait slices are one second. JSON bodies are at most 8 KB and use a
24 KB bounded allocator; firmware/image bodies are streamed. Slow headers,
bodies and readers must be exercised on hardware. The configured ten lwIP
sockets must also be checked with all permitted services active.

The 24 KB JSON limit is per allocator. The HTTP document and its temporary
8 KB receive buffer can overlap the provider's separate 24 KB document/TLS
context; admission does not block basic configuration requests. This combined
peak is part of the pending heap/fragmentation acceptance.

## Web server and REST API

`esp_http_server` serves the embedded gzip Web UI without a CDN or internet
dependency. The server registers every route explicitly and reports registration
or worker startup failures.

| Method and endpoint | Purpose |
|---|---|
| GET `/api/v1/status`, `/schema`, `/settings` | Status (screen, own match, competition state, full build identity, heap/task minima), bounds/options and upload limits, redacted settings |
| GET `/api/v1/session` | `{password_set, valid}` for the token the request carries |
| GET `/api/v1/wifi`, `/selection` | Bounded scan/selection results |
| GET `/api/v1/fixtures?competition=N&source=primary\|fallback` | The club's matches of a competition for the pair picker |
| GET `/api/v1/images`, `/images/file?id=N\|stock=crest\|background[&r=rev]` | Image inventory with per-image revision; the JPEG itself |
| POST `/api/v1/session`, `/logout` | Open/close the mutation session |
| POST or PUT `/api/v1/settings` | Strictly typed settings patch |
| POST `/api/v1/export` | Explicit settings export |
| POST `/api/v1/wifi/scan`, `/data/select`, `/data/fixtures`, `/data/refresh` | Start asynchronous scan, provider list, fallback fixtures or refresh |
| POST `/api/v1/display` | Quick display action `{"action":"next"\|"ip"}` (same events as the buttons) |
| POST `/api/v1/images/upload`, `/images/delete` | Upload a library image (optional `X-Image-Id` replaces one) or delete one by id |
| POST `/api/v1/images/repair` | Confirmed image-storage formatting and reference cleanup; preserves other settings and does not restart |
| POST `/api/v1/ota` | Stream compatible firmware to the inactive slot |
| POST `/api/v1/reboot`, `/reset` | Reboot or reset settings, optionally delete all images or format image storage |
| WebSocket `/api/v1/debug?token=` | Controlled live status while the Debug tab is open; ends with the session that opened it |

After the first full path, shortened paths in the table retain `/api/v1`.
See [WEB_UI.md](WEB_UI.md) for the browser workflow and image contract.

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
counter and must not be compared directly with the configured daily budget.

Image repair requires `{"confirm":"repair"}`, same-origin/session admission,
the heavy-operation token and no OTA trial. It formats LittleFS (including
stock files), then saves cleaned image references only if necessary. Success
returns `{"ok":true,"storage_formatted":true}`. A failed reference save after
formatting returns HTTP 500, `code:"repair_references"` and
`storage_formatted:true`; the filesystem operation already happened and must
not be mistaken for a fully successful repair. Neither result resets other
settings or restarts. USB `uploadfs` restores stock files.

### Provider-operation error keys

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
| `budget_day`, `busy` | Daily budget spent or another heavy operation prevented admission |
| `net_lost`, `time_invalid`, `settings_changed` | Network, clock or settings invalidated the operation |
| `rate_limited`, `auth`, `not_found`, `http_error` | HTTP 429, 401/403, 404 or another non-200 response |
| `connect` | No HTTP response; connection failed before a response |
| `bad_response`, `too_large`, `memory` | Unreadable/incomplete result, exceeded capacity or allocation failure |
| `other` | An unclassified failure; inspect the diagnostic text/log |

### Sessions

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

`GET /status` reports the numbers needed to judge memory under load without a
serial port: `heap_free`, `heap_largest` and `heap_min` (lowest free heap since
boot, internal 8-bit memory) plus `tasks`, an array of `{name, id,
stack_min_free}` with the lowest free stack in bytes of every task (at most 16).

### Errors

Every failed request answers `{"error": "<English text>", "code": "<stable
code>"[, "field": "<name>"]}` with `Cache-Control: no-store`. `code` is for
programs, `error` for people, `field` names the one value that caused a 400:
the dotted settings path (for example `competitions[0].primary.competition`),
a body key (`competition`, `season`, `id`, `confirm`) or a header
(`X-Image-Id`). For value errors `code` is the settings reason (`invalid`,
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

The browser uses local HTTP. Passwords and API tokens sent to this device are
therefore intended for its trusted local/setup network; TLS is used for provider
requests. No certificate or local HTTPS service is installed.

## OTA and rollback

The firmware contains a 92-byte versioned identity descriptor after the ESP
application descriptor. Upload checks cover project name, chip, target profile,
`4mb_ota_littlefs_v1` layout, a settings format equal to or newer than the running one and slot size before writing;
ESP-IDF validates the final image. A malformed/truncated upload does not select
the new slot. Compatible development versions are allowed in either direction;
there is no enforced monotonic version counter.

A pending trial has ten seconds to pass local health: initialized event bus,
settings, filesystem, display/UI, HTTP and local network services plus the
application-loop watchdog. Router connectivity, IP, SNTP and provider success
are not prerequisites. Failed local health explicitly invalidates/reboots the
image; if no previous bootable image exists, the fallback is a reboot path,
not a promised recovery image.

Valid format-1 and format-2 settings are converted in RAM without a write. The
next explicit save writes format 3, which an older firmware cannot read. Browser
OTA therefore refuses an image with an older settings format and accepts an
equal or newer one (a newer image migrates the stored settings). The first
format-3 image is installed by USB, because the running firmware's gate still
demands an equal format. A manual USB downgrade after a new save selects
old-firmware initial values.

NVS and LittleFS are shared between slots. Until trial confirmation, persistent
settings/image operations and durable provider-budget spending are blocked.
Firmware rollback cannot undo shared storage writes. Routine accepted firmware
OTA preserves image storage and compatible settings. USB `uploadfs` replaces
the filesystem image and can erase user uploads.

The first candidate installation uses USB: the older core image lacks the new
identity descriptor and is not a valid browser OTA input for this implementation.
Its SDK rollback role is separate from the new upload compatibility check.

Wrong-profile/layout images, interrupted uploads, power loss, local offline
health and actual rollback are required acceptance checks. None has been run
for this candidate.
