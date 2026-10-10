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

The C6 hardware profile selects GPIO3/GPIO14 before RF start and on antenna
changes, including the vendor's 100 ms settling interval. See
[HARDWARE.md](HARDWARE.md). Actual internal/external antenna reception remains
unverified. The Waveshare S3 profile is paused; no previously reported WiFi
power-save workaround has been added without current device evidence.

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
wait slices, no automatic redirects, an 8 KB incoming-header cap and body/wire
caps described in [DATA_PROVIDERS.md](DATA_PROVIDERS.md). DNS runs asynchronously
in the existing lwIP thread with one fixed, token-protected hostname/result
record. Late callbacks cannot reference a dead caller or overwrite a newer job.
TLS uses incremental nonblocking connection/handshake calls; the original host
is retained for certificate verification and SNI after numeric IPv4 resolution.
Cancellation checks run between steps for time, settings generation and network
epoch. SDK scheduling and cryptographic step duration still require measurement;
the implementation does not claim a measured hard real-time bound.

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
| GET `/api/v1/status`, `/schema`, `/settings` | Status, bounds/options and redacted settings |
| GET `/api/v1/wifi`, `/selection`, `/images` | Bounded scan/selection results and image inventory |
| POST `/api/v1/session`, `/logout` | Open/close the mutation session |
| POST or PUT `/api/v1/settings` | Strictly typed settings patch |
| POST `/api/v1/export` | Explicit settings export |
| POST `/api/v1/wifi/scan`, `/data/select`, `/data/refresh` | Start asynchronous scan, selection or refresh |
| POST `/api/v1/images/upload`, `/images/reset` | Upload/reset one slot or explicitly format image storage |
| POST `/api/v1/ota` | Stream compatible firmware to the inactive slot |
| POST `/api/v1/reboot`, `/reset` | Reboot or reset settings, optionally images |
| WebSocket `/api/v1/debug` | Controlled live status while the Debug tab is open |

After the first full path, shortened paths in the table retain `/api/v1`.
See [WEB_UI.md](WEB_UI.md) for the browser workflow and image contract.

The admin password is optional. Mutations nevertheless require a random
128-bit session nonce and same-origin/Host validation, including login, to
reduce unintended browser requests. A configured password is checked before
issuing the nonce. Sessions expire after 15 minutes. GET settings/status do not
return passwords or API keys; explicit export is a protected mutation.
Unknown keys, wrong types, excess arrays, embedded NULs and trailing non-space
JSON bytes are rejected. API keys are not passed into the debug producer.

The browser uses local HTTP. Passwords and API tokens sent to this device are
therefore intended for its trusted local/setup network; TLS is used for provider
requests. No certificate or local HTTPS service is installed.

## OTA and rollback

The firmware contains a 92-byte versioned identity descriptor after the ESP
application descriptor. Upload checks cover project name, chip, target profile,
`4mb_ota_littlefs_v1` layout, supported settings format 2 and slot size before writing;
ESP-IDF validates the final image. A malformed/truncated upload does not select
the new slot. Compatible development versions are allowed in either direction;
there is no enforced monotonic version counter.

A pending trial has ten seconds to pass local health: initialized event bus,
settings, filesystem, display/UI, HTTP and local network services plus the
application-loop watchdog. Router connectivity, IP, SNTP and provider success
are not prerequisites. Failed local health explicitly invalidates/reboots the
image; if no previous bootable image exists, the fallback is a reboot path,
not a promised recovery image.

Valid format-1 core settings are imported into RAM without a write. The next
explicit save writes format 2 because the expanded record exceeds the old
1 KB reader. Browser OTA requires matching format 2; a manual USB downgrade
after a new save selects old-firmware initial values.

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
