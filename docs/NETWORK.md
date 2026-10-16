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
A phone connected to the setup AP postpones home-network attempts, because
scanning can move the radio away from the AP channel, but for at most five
minutes (`kWifiApClientHoldMs`); after that the plan above continues while the
phone stays connected, and a phone that leaves and joins again starts a new
pause. The AP client count is reset whenever the setup AP is switched off. Existing scan cancellation and stale completion events are
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

A radio reconfiguration (WiFi credentials, AP password or antenna changed by
a save, and the first configuration at boot) stops the radio and applies
antenna, mode, station and AP configuration and start. If the WiFi driver
refuses one of these steps, the device logs `radio <step>: <error>` and
retries the whole reconfiguration every 10 seconds (`kWifiRadioRetryMs`);
it does not reboot, because the stored settings would meet the same error at
boot. Until a retry succeeds the local network counts as unhealthy, so an OTA
trial in that state is not confirmed.
The stop result is checked before reconfiguration. A failed stop retains
the AP/client state and applied settings and retries without configuring a
still-running radio. A successful stop clears the AP/client state before
antenna, mode or configuration calls can fail; only a successful start can
report the new AP as active.

The other driver calls that can fail at run time are retried on the same
10-second delay, without a reboot and without touching a working state:
- Switching the setup AP on or off (`mode: <error>`). The reported AP state
  (`ap` in the status) is the applied one: it changes only after the driver
  accepted the new mode, so a failed start does not show a setup network
  that is not there, and a failed stop does not hide one that still runs.
- Setting the hostname for DHCP and mDNS (`hostname: <error>`). Until it
  succeeds, DHCP and mDNS can temporarily use different names. On an
  allocation failure, the DHCP interface can fall back to the SDK's default
  hostname; retaining the previous name is not guaranteed.
- Starting the SNTP client (`SNTP init: <error>`), while the station is
  connected.

If the station lost and regained its IP (or the reverse) before the
connection manager handled the first event, only the later event counts.

A failed scan-result read releases the driver's scan list, as does a scan
that ends after it was cancelled; the driver holds that list in heap until
it is read or released.

Local initialization is retried by the existing application loop. Each
confirmed step retains its resources; only failed steps are retried, with
at least ten seconds between automatic retry batches. New settings are
adopted through the same path. Retry requires the existing nonblocking
heavy-operation token, so it waits while TLS, upload or another admitted
operation holds that token. A portal-DNS or mDNS failure does
not stop working WiFi, the setup AP or HTTP. Initial hostname assignment
and later hostname changes share the same acknowledged retry path; HTTP
advertisement follows confirmed mDNS hostname assignment. No additional
application task, second settings model or new stored setting is introduced.

Network-interface creation uses error-returning SDK operations instead of
convenience constructors that abort on failure. Failed partial interface
construction releases only that interface's owned resources. The global
TCP/IP stack and a pre-existing default event loop are retained. The portal
DNS worker starts only after its socket is configured and bound; failure
during socket setup closes that socket. If only task creation fails, the
one configured, bound socket is retained for retry; no second live worker
is created.

Recovery concerns errors returned by the SDK. Lower-level SDK assertions
remain fatal: for example, lwIP's first TCP/IP mailbox or core-lock
allocation may assert inside `tcpip_init()` rather than return an error.
The lifecycle cannot retry after such a process-ending SDK panic; no SDK
source or dependency is changed to claim otherwise.

`network_init` in the existing status API reports whether all local phases
are confirmed, the first active failed phase, its SDK error and short code,
and the next retry's monotonic time. Other independent phases can finish
while that failure remains visible. A recovered phase clears its failure;
a later failure is not hidden by historical errors. Local readiness requires
the radio, portal DNS, mDNS, current hostname and HTTP advertisement, not
Internet access or successful SNTP. See [API.md](API.md) for the fields and
[UI.md](UI.md) for the display behavior.

The ordinary `Kein WLAN` / `No WiFi` state still means that the connection
manager has not published a station IP lease. Initialization failures have
separate actionable messages in the Web UI and display, including their
short codes; the boot log also reports `boot: network init: <error>`.

A bounded UDP DNS responder directs captive-portal lookups to the AP address.
It answers standard queries with one question of class IN: type A with the AP
address, every other type (AAAA, HTTPS) with an empty answer, so a phone does
not wait for those lookups to time out. Additional records of a query, such
as an EDNS option, are not answered. Its socket is opened before its static
2 KB task starts; the 512-byte query buffer is static, not on that stack;
startup failure is reported. The full Web UI and firmware upload are available in AP mode.
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
mode. Clock validity starts false on every boot. If a new zone cannot be
applied (no memory for the rule), the previous zone stays and the time
service retries every 10 seconds (`kTimeZoneRetryMs`).

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
validation/publication/deletion, screenshot capture and OTA. Competing heavy operations return
HTTP 409 or wait for a later scheduled provider attempt: a provider request
that finds the guard taken is not sent, counts neither as a request nor as
a failure, and the scheduler offers it again after the 2-second device gap. Normal screen redraw,
including its bounded JPEG decoder, is still allowed during provider traffic;
that overlap must be measured.

A provider operation has a 30-second monotonic deadline, one-second socket
wait slices, an 8-second limit for provider silence while sending a request or
receiving its response, no automatic redirects, an 8 KB incoming-header cap and
body/wire caps described in [DATA_FLOW.md](DATA_FLOW.md). DNS runs
asynchronously in the existing lwIP thread with one fixed, token-protected
hostname/result record. Late callbacks cannot reference a dead caller or
overwrite a newer job. TLS uses incremental nonblocking connection/handshake
calls; the socket stays nonblocking afterwards, so reads and writes wait in
slices for a complete TLS record or send space. The original host is retained
for certificate verification and SNI after numeric IPv4 resolution.
Cancellation checks run between steps for time, provider-data settings
generation and network epoch. SDK scheduling and cryptographic step duration still require measurement;
the implementation does not claim a measured hard real-time bound.

Every provider request writes one serial log line (tag `provider`): either
`not sent: <URL> (<reason>)` when it stops before the connection (provider not
enabled, provider settings changed, network lost or changed, operation busy,
`daily budget used` when the provider's daily cap is reached, or `budget
unavailable` when its request counter could not be stored), or `<URL>: HTTP <status>, <bytes> B,
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

Screenshot responses additionally share a five-second C6 capture deadline
across producer waits and every underlying partial socket send. A screenshot
send override is scoped to its connection, which is closed after completion
or failure. The producer never sends on the HTTP task's behalf or waits for
queued work there. Cleanup detaches its copy buffer before scheduling the
connection close. This does not establish slow-reader or watchdog timing on
the device. The existing ordinary-response timeout behavior is unchanged.

The 24 KB JSON limit is per allocator. The HTTP document and its temporary
8 KB receive buffer can overlap the provider's separate 24 KB document/TLS
context; admission does not block basic configuration requests. This combined
peak is part of the pending heap/fragmentation acceptance.

## Web server and REST API

`esp_http_server` serves the embedded gzip Web UI without a CDN or internet
dependency. The server registers every route explicitly and reports registration
or worker startup failures.

All endpoints, sessions, status fields and error codes are described in
[API.md](API.md).

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
settings, filesystem, display/UI, HTTP and local network services (including
a configured radio) plus the
application-loop watchdog. Router connectivity, IP, SNTP and provider success
are not prerequisites. Any local network initialization error remains a
startup veto for that particular trial even if retries restore all network
services later. Recovery during a normal boot remains available. Failed
local health explicitly invalidates/reboots the
image; if no previous bootable image exists, the fallback is a reboot path,
not a promised recovery image.

Settings of an older format are not read: the device reports them as
outdated and runs on initial values until the owner sets it up again; the
next explicit save writes the current format, which an older firmware cannot
read. Browser OTA therefore refuses an image with an older settings format
and accepts an equal or newer one (a newer image discards older stored
settings, see CONFIGURATION.md). Historical: the first format-3 image had to be installed by USB,
because the format-2 firmware's gate still demanded an equal format. A manual USB downgrade after a new save selects
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
