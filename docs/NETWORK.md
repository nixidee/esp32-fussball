# Network — WiFi, setup AP, Web server, OTA

> Status: **planned** (P3). WiFi behaviour decided 2026-10-08. All timing values become defaults/limits in
> `include/app_config.h`.

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
- Disconnect reasons are classified with the IDF enum names
  (`WIFI_REASON_*`), never literal numbers (numbers changed between IDF
  versions).
- **AP client connected:** background scans paused (scanning makes the AP
  hop channels and disconnects phones).
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
SNTP after first connection; time zone from settings (default
`Europe/Berlin`, overridable in `secrets.h` and the Web UI). Matchday logic and night mode wait for
valid time.

## Web server and REST API (P3.5)
- `esp_http_server`; Web UI embedded in the firmware (gzip).
- API under `/api/v1/`. Planned groups: `status`, `settings` (schema,
  get, put), `wifi` (scan, save), `data` (competitions, teams),
  `images`, `ota`, `system` (reboot, factory reset).
- Request size limits per endpoint; input validation server-side.
- Optional admin password (default off); when set it also protects OTA.

## OTA (P3.7)
- Firmware upload in the Web UI (Update tab), streamed into the inactive OTA
  slot.
- Checks before switching: chip type, project name, **target profile id**
  (wrong profile's firmware is rejected), version.
- Rollback: the new firmware must confirm itself after boot (network and UI
  healthy); otherwise the bootloader returns to the previous version.
- Available in STA and AP mode.
- Password: optional (admin password, see above).
- Filesystem: own images are uploaded via the Web UI; the default images
  are updated only over USB (`uploadfs`).
