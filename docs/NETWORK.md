# Network — WiFi, setup AP, Web server, OTA

> Status: **planned**, not implemented. WiFi behaviour decided 2026-10-08.
> All timing values become defaults/limits in `include/app_config.h`.

## WiFi connection manager (P3.1)
Explicit state machine; decision logic is separate from the ESP-IDF driver
so it can be host-tested.

| State | Meaning |
|---|---|
| `AP_ONLY` | No credentials stored or in `secrets.h` → setup AP |
| `SCANNING` | Scan for the configured SSID |
| `CONNECTING` | Connect to the found network |
| `CONNECTED` | Got an IP (`IP_EVENT_STA_GOT_IP` = success, not the association) |
| `BACKOFF_AUTH` | Authentication failed; retry after 10 s, 30 s, 60 s |
| `WAIT_SSID` | SSID not visible; rescan every 20 s |
| `AP_FALLBACK` | Setup AP active, STA retries in the background |

### Rules
- **Start with credentials:** scan → if SSID visible: connect, several
  attempts with backoff → if it keeps failing: AP fallback.
- **Connection lost:** reconnect immediately. Short glitches never open the AP.
- **Auth failure** (wrong password, handshake timeout): retry after 10 s,
  30 s, 60 s. After that: AP fallback with background retries every
  5 min (default).
- **SSID gone:** rescan every 20 s; not back after 5 min → AP fallback,
  continue scanning in the background, reconnect when it appears.
- **Other reasons:** exponential backoff with jitter (bounded).
- **Never hang:** every wait has a timeout; no blocking loops.
- Association without a DHCP lease is not success. Bound the wait for an IP
  address and define recovery without leaving the state machine stuck.
- Scan completion, failure and cancellation are separate outcomes. A new
  credential selection cancels the previous scan/connection attempt; stale
  callbacks cannot restore the previous selection.
- Disconnect reasons are classified with the IDF enum names
  (`WIFI_REASON_*`), never literal numbers (numbers changed between IDF
  versions).
- **AP client connected:** background scans paused (scanning makes the AP
  hop channels and disconnects phones).
- Resume background scanning after the AP client leaves, subject to the
  existing retry schedule; handle a scan already in progress when it arrives.
- **STA connected:** the setup AP is stopped; it is not kept open (no grace
  period — an open AP costs performance). The new IP is shown on the
  display (IP badge).
- **One network** is stored (no list of networks).
- **Credentials:** SSID 1–32 bytes, copied by length (a full 32-byte SSID is
  valid). Password empty (open network), an 8–63-character passphrase or 64
  hexadecimal characters (raw key). Other lengths are rejected, never truncated.
- Board-specific: Waveshare uses `WIFI_PS_NONE` (known quirk from earlier projects on this board, see [HARDWARE.md](HARDWARE.md)).
- Board-specific: XIAO C6 antenna selection (internal/external) is applied
  before WiFi starts; see [HARDWARE.md](HARDWARE.md) → “External antenna”.

## Setup access point (P3.3)
- SSID: `Fussball-XXXX` (chip-ID suffix, proposal).
- Security: password **optional** (default: open). If a password is set in
  the settings or `secrets.h`, the AP uses WPA2. Enforcing a password can
  be added later as a config option.
- Captive portal DNS: phones open the configuration page automatically.
- IP `192.168.4.1` (IDF default), shown on the display (IP badge).
- The complete Web UI is available in AP mode, including the Update tab.

## mDNS (P3.4)
Component `espressif/mdns` (accepted 2026-10-08). Hostname default `fussball`
(`app_config.h`) → `http://fussball.local`; overridable in `secrets.h`
(`SECRET_HOSTNAME`) and by the user in the Web UI (WiFi settings).
Precedence as in [CONFIGURATION.md](CONFIGURATION.md).

## Time

The time service is implemented without networking. SNTP connection and its
network device test are deferred to the WiFi manager (P3.1), where SNTP will
start after the first connection and use the stored NTP server (default
`pool.ntp.org`). No provisional WiFi stack or extra time task is introduced.

The settings store holds a location label (default `Europe/Berlin`), mapped
to one of 40 compiled POSIX `TZ` rules in `cfg::kTimeZones`. The list covers
Europe, the Americas, Africa, Asia, Australia and the Pacific, plus `UTC`.
Labels are stored as text, so reordering the list cannot change a saved
zone. `SECRET_TIME_ZONE` provides the optional location preset; the future
Web UI will use these same labels. See [CONFIGURATION.md](CONFIGURATION.md)
for limits and the migration from `SECRET_TIMEZONE`.

The service uses standard C-library `setenv("TZ", ...)`, `tzset()` and
`localtime_r()`; the current ESP-IDF firmware uses Picolibc. Only this
service may apply `TZ`, because it is global to the firmware. A mutex
serialises zone changes and conversions. The recurring rules derive from
IANA tzdata 2026c footers and were checked against 2026e for 2026–2028;
Dublin uses an equivalent positive-summer convention. No historical
time-zone database is installed, so past rule changes are not reproduced.

- Fixture instants are UTC. `toLocal()` converts known instants even while
  the device clock is invalid; `utcNow()` and `localNow()` refuse an invalid
  clock. Local civil time is used for display and night-mode windows.
- The clock is valid only after a time source sets it in the current boot.
  A reboot starts invalid even if hardware retained its raw clock value.
  Normal firmware currently has no synchronisation source: until SNTP is
  connected, date-dependent screens and night mode wait for valid time.
- `setTime()` sets UTC through `settimeofday()`, records the source and
  monotonic set time, and posts `kTimeChanged`. Applying a different zone
  also posts that notification. Subscribers fetch current state; events
  coalesce. The first set is logged; corrections larger than two seconds
  produce a jump warning.
- `monotonicMs()` uses `esp_timer_get_time()` for delays, retries, deadlines,
  freshness and input/overlay timeouts. Wall-clock corrections do not move
  it. Civil windows include the start minute and exclude the end; a start
  after the end crosses midnight, and equal endpoints mean an empty window.
  A skipped or repeated daylight-saving hour follows the local clock.

### Offline device test

Set `cfg::kTimeTest = TimeTest::kRulesAndJumps` in `app_config.h` for one
diagnostic build, then restore `kNone` and upload normal firmware. This test
uses no network and does not alter stored settings, but leaves the wall
clock valid at its synthetic 2030 test time until the next boot.

The test passed on the XIAO ESP32-C6 on 2026-10-09: invalid clock at boot;
112 rule cases covering all 40 zones and their 2026–2027 transitions;
12 actual `setTime()`/`localNow()` night-window fixtures covering the
23:00–07:00 boundaries, midnight and both daylight-saving changes; a 2030
baseline followed by a confirmed backward jump to 2020 and forward jump to
2030; and event delivery, validity/source/set counters and monotonic-clock
continuity for every set. Expected result: `PASS: boot invalid, 112 rule
cases, 12 window cases, 2 jumps`. There are 15 clock sets in total. Rule
checks took 25 ms. Heap moved from 418,456 B to 417,808 B while cycling every
zone (648 B diagnostic cost), which is separate from normal single-zone
boot usage.

Host tests exercise the pure helpers and rules through macOS's C library;
the device test verifies the firmware's Picolibc and actual clock APIs.
SNTP reception, reconnect behaviour and network-origin corrections remain
for P3.1; the offline test does not claim that acceptance.

## Web server and REST API (P3.5)
- `esp_http_server`; Web UI embedded in the firmware (gzip).
- API under `/api/v1/`. Planned groups: `status`, `settings` (schema,
  get, put), `wifi` (scan, save), `data` (competitions, teams),
  `images`, `ota`, `system` (reboot, factory reset).
- Request size limits per endpoint; input validation server-side.
- Optional admin password (default off); when set it also protects OTA.

All network services follow one coordinated, bounded operation policy
(admission of competing operations, deadlines, cancellation and limits; ADR-016).
Before implementation, specify and measure endpoint body/string/nesting/entity
limits, operation deadlines, retry/redirect bounds and concurrent connections.
Limits cover total elapsed time and all incoming bytes, including fields a
JSON filter discards. Competition/team selection and uploads need their own
bounds; one measured match response is not a budget for every endpoint.
Reserve capacity for configuration and uploads alongside provider traffic;
cancel affected operations when the network or settings generation changes.

When a password is configured, the session/logout contract protects mutations
and OTA consistently. GET requests remain read-only. Validate request origin
and uploads, and redact credentials/API keys from logs and responses. These
requirements retain the optional-password policy; exact session and limit
values are decided before implementing the affected service.

## OTA (P3.7)
- Firmware upload in the Web UI (Update tab), streamed into the inactive OTA
  slot.
- Checks before activation: image validity and size, chip type, project name,
  version policy, **target profile and partition-layout compatibility**.
  ESP-IDF's `esp_app_desc_t` has no target-profile field. Explicit compatibility
  metadata is required; a dedicated, versioned custom image descriptor is the
  chosen representation (ADR-016). Choose its layout and compatibility rules
  before implementation, then measure the cost.
- The trial image confirms itself only after a bounded **local boot health
  check**. Router access, an IP lease, SNTP or a successful provider request
  are not prerequisites. Healthy offline operation, including the setup AP
  path, must be possible. The local checks and deadline are decided before
  implementation.
- An unsuccessful check needs an explicit, bounded invalidation/reset path;
  rollback is not a timer that rescues firmware which hangs forever without
  resetting. Define recovery if no previous bootable image exists.
- NVS and LittleFS are shared by both app slots. Firmware rollback does not
  revert their contents. Trial firmware preserves image files readable by the
  previous image until acceptance. Settings are kept only while the settings
  format version is unchanged; see [CONFIGURATION.md](CONFIGURATION.md).
- Available in STA and AP mode.
- Password: optional (admin password, see above).
- Routine firmware OTA updates firmware and its embedded Web UI, preserving
  LittleFS images and settings of an unchanged settings format. USB `uploadfs` is a development operation
  that replaces the filesystem image and may erase user images; the image
  storage/update policy is in [WEB_UI.md](WEB_UI.md).

Acceptance covers wrong-chip/profile/layout images, malformed or truncated
uploads, power loss, local healthy boot without a router/provider, failed boot
and rollback to an older image; after a settings format change the older
image starts with defaults.
