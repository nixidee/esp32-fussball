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
SNTP starts after the first connection. The settings UI presents a location
label such as the default `Europe/Berlin`; the time service maps supported
labels to POSIX `TZ` rules. ESP-IDF receives the POSIX rule, not an IANA label
that would require an installed time-zone database. The zone is overridable
in `secrets.h` and the Web UI.

- Fixture times use UTC. Local civil time is used for display and night-mode
  windows, including windows that cross midnight.
- Retry delays, freshness durations, input/overlay timeouts and operation
  deadlines use a monotonic clock, independent of SNTP corrections.
- Date-dependent logic waits for valid wall-clock time. Invalid time, forward
  and backward SNTP corrections, daylight-saving changes and midnight-crossing
  night windows are required test cases.

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
