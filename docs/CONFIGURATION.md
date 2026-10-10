# Configuration

Implementation state: 0.1.0-dev, 2026-10-10. All planned settings groups are
connected to the device services and offline Web UI. The current C6 build
compiles; new fields, persistence interruptions and runtime controls still
require acceptance. Historical core-service tests do not certify this extension.

## Three sources, one place each

| What | Where | In Git |
|---|---|---|
| Hardware facts | `boards/`, `displays/`, `targets/` | yes |
| Software defaults, limits and enums | `include/app_config.h` | yes |
| Personal bootstrap presets | `include/secrets.h` | no |
| Runtime configuration | NVS namespace `settings`, key `record` | device only |

The template `include/secrets.h.example` is tracked. A stored record overrides
bootstrap presets; omitted appended fields use their initial preset/default.
An invalid preset is skipped and its field name is logged without its value.
Settings are typed fixed storage in `components/settings/`; the NVS adapter
is `components/core/settings_store.cpp`. Successful save/reset increments
the settings generation and posts a short change notification.

## Optional bootstrap

Copy `include/secrets.h.example` to `include/secrets.h` if first setup should
already know the network or club. Every key is optional. Without credentials
the setup AP starts; without an enabled competition the stock image is shown.
Presets are compiled into the firmware, so a personal binary must also be
treated as containing personal credentials.

Supported keys are `SECRET_WIFI_SSID`, `SECRET_WIFI_PASSWORD`,
`SECRET_AP_PASSWORD`, `SECRET_HOSTNAME`, `SECRET_TIME_ZONE`,
`SECRET_ADMIN_PASSWORD`, `SECRET_API_FOOTBALL_KEY`,
`SECRET_FOOTBALL_DATA_KEY`, `SECRET_DEFAULT_PROVIDER`,
`SECRET_DEFAULT_COMPETITION`, `SECRET_DEFAULT_TEAM` and
`SECRET_DEFAULT_SEASON`. Provider values are `openligadb`, `api-football`,
`espn` or `football-data`. ESPN additionally requires explicit runtime opt-in.
Competition/team IDs always belong to that provider. A season is a provider
season, rather than a year inferred from the device's calendar.

Former `SECRET_WIFI_SSID_1`/`SECRET_WIFI_PASSWORD_1` and `SECRET_TIMEZONE`
names deliberately stop the build. The time-zone preset is a supported
location label, such as `Europe/Berlin`, rather than a POSIX rule.

## Defaults and bounds

The Web UI reads `GET /api/v1/schema`; it does not maintain another defaults
table. Partial settings requests retain omitted fields, including secrets.
Unknown keys, incorrect types, malformed array lengths and invalid values
are rejected before a whole-model save.

| Group / field | Default | Accepted range or meaning |
|---|---|---|
| WiFi SSID | empty | 0–32 bytes; empty selects setup |
| Station password | empty | open, 8–63 printable ASCII characters, or 64 hexadecimal digits |
| Setup AP password | empty | open, or 8–63 printable ASCII characters |
| Hostname | `fussball` | one DNS label, 1–63 characters |
| External antenna | off | boards with an antenna switch; reconnects the radio |
| Time zone / NTP | `Europe/Berlin` / `pool.ntp.org` | 40 compiled location labels / valid DNS name up to 63 characters |
| Admin password | empty | up to 64 bytes, no embedded NUL; empty still uses mutation sessions |
| API-Football / football-data keys | empty | up to 96 / 64 printable non-space ASCII bytes |
| Primary routes | disabled, OpenLigaDB | three routes: league and two optional competitions |
| Route IDs / season | empty | competition/team up to 32 URL-safe characters; season up to 16 decimal digits for OpenLigaDB, four digits for other providers |
| API-Football season | required on an enabled route | explicit year; other providers allow current-season selection |
| Fallback routes / fixture pairs | disabled | three alternate routes, eight explicitly verified pairs |
| ESPN / demo / browser diagnostics | off | explicit switches |
| Screens / backgrounds | all enabled / on | four styles; at least one screen enabled |
| Background / text / accent | `#101820` / `#ffffff` / `#49cba0` | RGB colours per screen |
| Text scale | 100% | 70–130%, selects bounded compiled font roles |
| Defaults outside / matchday / own game | Table / Conference / Own match | enabled available screen |
| Manual return / scroll-direction reset | 60 / 20 seconds | 0–4000; 0 disables the corresponding timeout |
| Scroll repetition | 250 ms | 100–2000 ms |
| Visible conference / table rows | 5 / 5 | 1–5 / 1–9; all stored table rows remain reachable |
| Slideshow | 15 seconds | 3–3600 seconds; up to five uploaded slides |
| Brightness / night brightness | 100% / 10% | 0–100%; hardware dimming requires a backlight pin |
| Rotation | 0 | 0–3 quarter turns |
| Language | German | German or English |
| Night mode / window | on / 23:00–07:00 | local civil time; equal endpoints mean empty; requires valid clock |
| IP badge | 60 seconds, top | 0–4000 seconds, 0 permanent; setup AP permanent by default |
| Match window before / after | 30 / 30 minutes | 0–180 minutes |
| API-Football daily budget | 100 requests | 1–10000; durable reservations count every attempted request |
| Console status | on, 30 seconds | existing stored interval limits; browser stream is separate |

Screen indices are Own match 0, Conference 1, Table 2, Crest/slideshow 3.
Provider indices are OpenLigaDB 0, API-Football 1, ESPN 2, football-data 3.
A fallback must use a different provider. Its configured competition, season
and club plus each enabled primary/secondary fixture ID explicitly certify
the pairing; `swapped` declares opposite home/away order. No name matching
creates a pairing.

## Record format and consistent saves

Current format is **2**, with explicit little-endian encoding, not a structure
memory dump. Current record size is **1720 bytes**, payload 1712 bytes;
the reader accepts at most 2048 bytes. Header: 16-bit format, 16-bit payload
length and IEEE CRC32 over the first four bytes plus payload.

The original payload offsets through byte 334 are preserved. The header is
regenerated for format 2, the new length and CRC:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | header |
| 8 | 33 | length-prefixed WiFi SSID |
| 41 | 65 | station password |
| 106 | 64 | AP password |
| 170 | 64 | hostname |
| 234 | 1 | antenna |
| 235 | 1 | console status |
| 236 | 2 | console interval |
| 238 | 33 | time-zone label |
| 271 | 64 | NTP server |
| 335 | 227 | admin and two API credentials |
| 562 | 255 | three primary routes, 85 bytes each |
| 817 | 60 | four styles, 15 bytes each |
| 877 | 7 | application switches |
| 884 | 9 | language, brightness, rotation, counts and screen defaults |
| 893 | 20 | ten 16-bit timing/budget values |
| 913 | 255 | three fallback routes |
| 1168 | 552 | eight fixture pairs, 69 bytes each |

A missing tail at a field boundary retains initial values. Ending inside a
field fails; an unknown same-format tail is ignored within the reader limit.
The decoder also accepts valid legacy format-1 records up to the old 1024-byte
limit. It imports only their known core through byte 334; unknown legacy tails
are ignored after the CRC check. The 238- and 335-byte core layouts therefore
remain readable, and newly added fields keep initial values. Loading never
rewrites NVS. The next explicit save, after any OTA trial, writes format 2.

Format 2 is necessary because the 1720-byte record exceeds the old firmware's
1024-byte read ceiling. An append can retain its format version only within the
previous reader's supported size. Reordering/type/meaning changes or crossing
that ceiling require a version/compatibility decision. The old format-1 firmware
cannot read a format-2 record and uses initial values after a manual downgrade.
No reverse migration or automatic downgrade write is performed.

A whole record is written and committed before RAM settings change. NVS
provides atomic blob replacement; interruption acceptance remains a separate
physical test. Loading checks size, length, CRC, format and value limits.
Damaged/unknown records select initial values and remain stored until save
or reset. An unreadable record prevents saving data that was never checked.

The established NVS startup recovery erases the entire NVS partition only
for `NO_FREE_PAGES` or `NEW_VERSION_FOUND`, with an error log. That also
clears radio calibration and provider-budget records. Other NVS errors
retain RAM defaults and refuse persistent saves.

## Reset and firmware updates

System reset always clears the `settings` namespace and restarts. User images
are retained unless the explicit image-deletion option is selected. The separate
full image-storage repair option formats LittleFS, including stock files.
It requires explicit confirmation and never follows automatically from a
mount fault. Stock files then need initial USB `uploadfs`; custom files can
be uploaded through the Web UI.

On the current three-input C6, holding **all three inputs for eight seconds**
resets settings and restarts. The badge shows a countdown. Release cancels the
gesture; after triggering it stays latched until release. This gesture does
not erase images. It is disabled during an OTA trial.

The provider-budget namespace is retained by normal settings/image resets.
Its 24-byte durable ledger reserves blocks of up to eight attempts before
network I/O. Unused reserved credits are lost on reboot; a backward clock
correction does not replenish them. Advancing to a later UTC day starts a
new ledger. Failed storage refuses provider requests rather than bypassing
the budget. This is a local limit, not a shared-IP provider quota guarantee.

Routine firmware OTA preserves LittleFS. The current browser OTA gate accepts
only the same supported settings format 2. Future format transitions need an
explicit compatibility/import policy. A USB downgrade to the old core firmware
selects its initial values once format 2 has been saved. Trial firmware refuses
settings and image writes until local boot acceptance, so automatic rollback
can still read the untouched legacy record. Rollback does not undo shared
storage after an accepted image has subsequently saved new settings.

## Resources and acceptance

C6 compiler sizes: Model 1736 bytes, encoded record 1720 bytes. The stored
model and network's applied copy are fixed RAM; readers take bounded stack
copies. Save allocates the record temporarily; load allocates at most 2048
bytes. These costs are additional to NVS page/index heap. A 1720-byte blob
uses roughly 56 NVS entries, about 112 while replacing it; these are layout
estimates, not measured full-storage acceptance. The partition remains 20 KB.

The earlier 238-byte layout passed C6 store selectors on 2026-10-09 and
30 reset-button interruptions; supply removal remains untested. The later
335-byte layout passed host compatibility tests. Neither result certifies
the current 1720-byte extension. Current store selectors remain available
in `app_config.h`, normally `kNone`. New settings, old/new record tails,
full/corrupt storage, real power loss, resets and OTA rollback still require
tests. No such tests or device upload occurred for this candidate.
