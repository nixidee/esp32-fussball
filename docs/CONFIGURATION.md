# Configuration

> Status: **partly implemented.** The settings store (one NVS record with
> format 1: WiFi, setup AP password, hostname, external antenna, status log,
> time zone and NTP server)
> and the `secrets.h` presets work; nothing uses the WiFi values yet, the Web
> UI and the reset trigger are planned. Further settings are appended to the
> record by the features that need them. NVS was selected in ADR-007; the
> consistency and OTA storage contract is refined by ADR-011.

## Three sources, one place each
| What | Where | In Git |
|---|---|---|
| Hardware facts (pins, display, buttons) | `boards/`, `displays/`, `targets/` | yes |
| Software defaults, limits, enums (one structured file) | `include/app_config.h` | yes |
| Secrets and personal presets (WiFi, API keys, club, location) | `include/secrets.h` | **no** (template `include/secrets.h.example` is tracked) |

Runtime settings are stored on the device in NVS (ESP-IDF's key-value store
in flash, ADR-007) and override the defaults. Code: the pure model, limits
and record format in `components/settings/` (host-tested), the NVS store in
`components/core/` (`settings_store.h`).

## Precedence
The boot layout uses compile-time tokens from `app_config.h`: content margin,
diagnostic ring width/gap and preferred text width. These are not stored
settings or Web controls. For runtime settings, per field:

1. Value stored on the device (set via Web UI)
2. `secrets.h` value, used while no stored record exists (first boot, after a
   reset) and for fields a stored record does not contain yet
3. Default from `app_config.h`

A `secrets.h` value that violates its limits is skipped with an error log
naming the key (never the value); the default applies.

Example: mDNS hostname — default `fussball` in `app_config.h`, overridden by
`SECRET_HOSTNAME` if set, overridden by the Web UI value once saved.

## `secrets.h`
1. Copy `include/secrets.h.example` to `include/secrets.h`.
2. Fill in your values. Empty values mean “not set”.
3. Never commit `secrets.h` (it is in `.gitignore`).

Note: values from `secrets.h` are compiled into the firmware binary. Do not
share firmware files built with your personal `secrets.h`.

`secrets.h` is **optional** (ADR-007), and so is every key in it: a missing
file or key counts as "not set". The template marks which keys the firmware
reads today. Missing values fall back to their defaults in `app_config.h`;
once networking and the data providers are implemented this means:
- no WiFi credentials → the device starts the setup AP;
- no club / league → a default image is shown instead of football data.
It only makes the first setup more comfortable.

The WiFi keys are `SECRET_WIFI_SSID` and `SECRET_WIFI_PASSWORD`. The former
names `SECRET_WIFI_SSID_1` / `SECRET_WIFI_PASSWORD_1` stop the build with a
message asking to rename them. The time-zone key is `SECRET_TIME_ZONE` and
must contain one supported location label, such as `Europe/Berlin`, rather
than a POSIX rule. The former `SECRET_TIMEZONE` name stops the build with a
rename message. Renaming the key does not convert an existing POSIX value;
replace that value with a supported location label. An invalid preset logs
the field name and keeps the Berlin default.

## Factory reset
Reset erases the complete stored settings record (the whole NVS namespace
`settings`); the `secrets.h` values and defaults apply immediately. The store
function exists; the Web UI trigger and the restart that follows it are
planned. No stale
setting may reappear after a reset or a settings format change.
Deletion of user images is an explicit optional reset choice. A mount fault
must never be interpreted as permission to format LittleFS or discard images.
Reset is triggered from the Web UI (System); a device trigger must also remain
possible, with its exact input gesture to be specified before implementation.

## Storage, consistent saves and firmware updates

Settings survive every restart and power loss. The complete settings model is
stored as **one NVS record** (a "blob") with a small header: format version,
length and a CRC32 check value. ESP-IDF's NVS writes a changed blob completely
before it removes the old one and discards incomplete blobs at start-up, so
after a power loss the device finds either the old or the new complete model,
never a mixture. Software defaults and limits stay in `app_config.h`.

At start-up the record is loaded and checked (version, length, check value,
limits). A missing, damaged or unknown record means: `secrets.h` values and
defaults. Saving writes the whole record at once.

The record is **append-only**: a new setting is added at the end of a fixed,
explicitly defined layout (not a compiler-dependent structure image). A shorter
record of the same format version is accepted and the missing new fields get
their defaults; a longer record (written by newer firmware, e.g. before a
rollback) is accepted and the unknown tail is ignored. Only reordering,
removing or changing the meaning or type of an existing field increases the
format version.

Firmware updates do **not** guarantee that settings are kept. Settings are
carried over when the new firmware uses the same settings format version
(adding settings does not change it), also after a rollback to older
firmware of the same format version. After a format version change, in
either direction, the device starts with `secrets.h` values and defaults and
must be configured again (without WiFi credentials in `secrets.h` this means
the setup access point).
The version in the header allows a migration to be added later without
changing the stored format. Image files on LittleFS are a separate matter and
are kept by routine firmware updates (see [WEB_UI.md](WEB_UI.md)).

Save and reset report failure rather than claiming success after a partial
operation. Required tests include interruption of a save, a damaged or full
NVS, an unknown format version, shorter and longer records of the same
version, and reset.

### Record format 1

NVS namespace `settings`, key `record`; all numbers little-endian. The
record is 335 bytes; at most 1,024 bytes are read (a longer record counts as
damaged).

| Offset | Size | Content |
|---|---|---|
| 0 | 2 | format version (1) |
| 2 | 2 | payload length (327) |
| 4 | 4 | CRC32 (IEEE, as zlib) over bytes 0–3 and the payload |
| 8 | 1 + 32 | WiFi SSID: length byte, then the bytes |
| 41 | 1 + 64 | WiFi password |
| 106 | 1 + 63 | setup AP password |
| 170 | 1 + 63 | hostname |
| 234 | 1 | external antenna (0/1) |
| 235 | 1 | periodic status log (0/1) |
| 236 | 2 | status log interval in seconds |
| 238 | 1 + 32 | time-zone location label |
| 271 | 1 + 63 | NTP server |

Unused bytes after a text are written as zero and ignored when read. The
checks run in this order: size, length field, check value, version, then
every field against its limits. A record that ends inside a field, or in
which one value violates its limits, is rejected as a whole; a record that
ends exactly between fields is accepted. Appending time zone and NTP server
keeps format version 1: an earlier 238-byte record remains valid and its new
fields use the initial values (`secrets.h` over defaults).

### Limits of format 1

| Setting | Default | Allowed values |
|---|---|---|
| WiFi SSID | empty (no network) | 0–32 arbitrary bytes |
| WiFi password | empty (open network) | empty, 8–63 printable ASCII characters, or exactly 64 hexadecimal characters |
| Setup AP password | empty (open AP) | empty or 8–63 printable ASCII characters |
| Hostname | `fussball` | 1–63 letters, digits or hyphens, no hyphen at the start or end |
| External antenna | off | on/off (used only on boards with an antenna switch) |
| Periodic status log | on | on/off |
| Status log interval | 30 s | 5–3,600 s |
| Time zone | `Europe/Berlin` | one of the 40 location labels in `cfg::kTimeZones`, case-sensitive; at most 32 characters |
| NTP server | `pool.ntp.org` | 1–63 letters, digits, hyphens and separating dots; no empty label or hyphen at a label's start/end; a DNS name or IPv4 address |

Values that do not fit are rejected, never truncated.

### Start-up and errors

- No stored record: initial values (`secrets.h` over defaults), logged.
- Damaged record, unknown version or a value outside its limits: an error log
  names the reason (and the field, never its value); initial values apply.
  The record stays in flash until the next save or reset replaces it.
- The record exists but cannot be read (flash read error, no memory for the
  read buffer): initial values apply and saving is refused until a reset, so
  data that was never checked is not overwritten.
- NVS reports "no free pages" or "new version found" at start-up (the two
  cases for which ESP-IDF requires an erase): the whole NVS partition is
  erased and initialised again, with an error log. This also deletes the
  radio calibration data, which the radio measures again. Any other NVS start
  error: no erase; the device runs with the initial values in RAM only and
  every save reports an error.
- A save that NVS cannot complete (for example NVS full) returns the error;
  the current settings stay unchanged.

The boot log shows the loaded settings with their source; the WiFi SSID and
the passwords appear only as "set"/"empty" (and the SSID length).

### Memory

The current settings occupy 328 bytes on the ESP32-C6 (verified in the
target linker map) and live in static RAM behind a mutex; readers get a copy
on their own stack. The time fields add 98 bytes per model, including one
alignment byte, and 97 bytes to the explicit record. Saving allocates the
335-byte record temporarily on the heap, loading at most 1,024 bytes. NVS
itself keeps its page and entry index on the heap; the boot log lines "heap
at boot" and "heap after settings init" show the amount.

Space in the NVS partition (20 KB = 5 pages of 126 entries of 32 bytes; one
page stays free for compaction): the 335-byte record takes about 13 entries
(blob index, data header, 11 data entries), about 26 while a save writes the
new copy before the old one is released. That is about 5 % of the roughly
500 usable entries; the WiFi driver uses the same partition. The earlier
238-byte record needed about 10 entries, so this extension adds about three
entries per stored copy. These counts are estimates from the NVS entry
format; the `kNvsFull` test logs the actual entry counts.

### Device tests

`cfg::kSettingsTest` in `app_config.h` compiles one store test into the boot
sequence (it must be `kNone` in every normal build). The tests that store a
prepared record change the hostname (`fussball-test`), the external antenna
(on), the status log (on), its interval (10 s), the time zone
(`Europe/London`) and the NTP server (`de.pool.ntp.org`); WiFi values are kept. A prepared record takes effect at the next boot (press RST); the result
is the settings line of that boot log.

| Value | Expected at the next boot |
|---|---|
| `kSaveSample` | "stored settings loaded", hostname `fussball-test`, interval 10 s, zone `Europe/London`, NTP `de.pool.ntp.org` |
| `kReset` | "settings reset" in the same boot; next boot "no stored settings" |
| `kCorruptRecord` | "stored settings damaged (… check value mismatch)", initial values |
| `kUnknownVersion` | "stored settings damaged (… unknown format version)", initial values |
| `kShorterRecord` | loaded; hostname `fussball-test`, interval 10 s, zone `Europe/London`; omitted NTP field uses `pool.ntp.org` |
| `kLongerRecord` | loaded; all sample values, the unknown tail is ignored |
| `kNvsFull` | same boot: "PASS: save failed …, settings unchanged", fill data removed |
| `kSaveLoop` | saves interval 100 s and 200 s alternately every 100 ms (2,000 times); after a power cut at any moment the next boot loads 100 or 200 s, never a damaged record |

All store test selectors passed on the XIAO ESP32-C6 on 2026-10-09 with the
earlier 238-byte layout. `kSaveLoop` was interrupted 30 times with the reset
button (every next boot loaded 100 or 200 s) and ran once to completion
(about 225 s); interruption by removing the supply has not been tested yet.
The 335-byte layout and compatibility with the earlier records pass the host
tests; the store's save/reboot tests have not been repeated for the new
layout. The earlier measured settings-store heap cost at boot was 2,168 B.

## Settings groups (planned)
| Group | Examples |
|---|---|
| Club / data | provider, API keys, competitions, club |
| Screens | default screen per situation (no matchday / matchday / own match), return-to-default time (default 60 s, or off), scroll direction reset time (default 20 s), scroll rate while held (default set in P5.4), colours, backgrounds, live list size, table window |
| Overlays | IP badge position, duration, permanent in AP mode |
| WiFi | one network (SSID, password), optional AP password, hostname, external antenna (boards with antenna switch; default off) |
| Display | brightness (only targets with a backlight pin), rotation, night mode window (default 23:00–07:00) |
| Time | time-zone location label mapped to a POSIX rule, NTP server; clock use is defined in [NETWORK.md](NETWORK.md) |
| System | optional admin password (also protects OTA; default off), factory reset |
| Debug | switches per debug output, e.g. periodic status log (stored, default on, interval 30 s) |

Each setting has a type, default, min/max (or allowed values) and a schema
version — all in `app_config.h`. The Web UI reads the schema from the device,
so defaults and limits exist only once.
