# Configuration

Implementation state: 0.1.0-dev, 2026-10-12 (settings format 3). All planned settings groups are
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
table. Every rejected value is answered with the name of its field and the
reason (see [NETWORK.md](NETWORK.md)). Partial settings requests retain omitted fields, including secrets.
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
| Competitions | empty list | ordered list of up to three entries, each active or paused; the first active entry supplies the league table |
| Competition source | OpenLigaDB | provider, competition ID and club ID up to 32 URL-safe characters; season up to 16 decimal digits for OpenLigaDB, four digits for other providers |
| API-Football season | required while its source is active | explicit year; other providers allow current-season selection |
| Fallback (per competition) | off | a second source of another provider, active or paused; a paused fallback may stay incomplete |
| Display names | empty | competition and club name stored when picked from a provider list, up to 32 bytes, no control characters |
| Fixture pairs | empty list | up to eight explicitly verified pairs, each referring to a competition by its list position |
| ESPN / demo / browser diagnostics | off | explicit switches |
| Screens / backgrounds | all enabled / on | four styles; at least one screen enabled |
| Background / text / accent | `#101820` / `#ffffff` / `#49cba0` | RGB colours per screen |
| Text scale | 100% | 70–130%, selects bounded compiled font roles |
| Defaults outside / matchday / own game | Table / Conference / Own match | enabled available screen |
| Manual return / scroll-direction reset | 60 / 20 seconds | 0–4000; 0 disables the corresponding timeout |
| Scroll repetition | 250 ms | 100–2000 ms |
| Visible conference / table rows | 5 / 5 (240 px round: 4 / 5) | 1–5 / 1–9 (240 px round: 1–4 / 1–7); the display profile sets the upper limits, the schema reports them, the API rejects more, stored values above them are clamped when loaded; all stored table rows remain reachable |
| Slideshow | 15 seconds, no slides | 3–3600 seconds; up to five slides in play order, each the crest or a library image |
| Image library | empty | up to ten uploaded JPEGs (ids 1–10); crest, screen backgrounds and slides refer to them, id 0 is the stock image |
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
Minimum request spacing is currently a compile-time policy in `app_config.h`:
5 / 6 / 30 / 6 seconds in that provider order. It applies to every request
start, including selection lists, and is separate from both the post-refresh
poll delay and API-Football's stored daily budget. It is not yet an editable
runtime setting.
An empty OpenLigaDB season means the current season: the competition list
uses the season named after the current year from `kSeasonStartMonth`
(July) on, before that the previous year's, and match requests let
OpenLigaDB report the current season. A season typed in the setup wizard's
season field is stored with the competition; picking from the current list
leaves it empty. WiFi modem sleep is fixed off by `kWifiPowerSave`.
On the current C6 without a backlight pin, night mode darkens background and
images. Night brightness is stored/editable, but foreground colour dimming
belongs to the still-unimplemented display redesign. A changed value is not
evidence that the physical backlight or text brightness has changed.
A fallback must use a different provider. Its configured competition, season
and club plus each active primary/secondary fixture ID explicitly certify
the pairing; `swapped` declares opposite home/away order. No name matching
creates a pairing. An active pair needs an active fallback on its competition,
and active pairs of one competition use distinct IDs on both sides.

Lists never contain empty entries: an entry exists or the list is shorter.
The crest screen shows the crest or its slides and has no background image,
so its `image` stays 0. A reference to a library image that no longer exists
is replaced by the stock image (a slide is removed) at the next boot outside an
OTA trial and when the image is deleted; the renderer skips such a reference
meanwhile.

## Record format and consistent saves

Current format is **3**, with explicit little-endian encoding, not a structure
memory dump. The record has a variable length: each list stores only its
entries. The longest possible record (every list full, every text at capacity)
is **1931 bytes**, pinned by a host test; a record without lists is 666 bytes.
The reader accepts at most 2048 bytes (`kSettingsMaxRecordBytes`). Header:
16-bit format, 16-bit payload length and IEEE CRC32 over the first four bytes
plus payload.

The original payload offsets through byte 334 are preserved in every format.
Text fields are one length byte followed by their capacity.

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
| 335 | 227 | admin password (65), API-Football key (97), football-data key (65) |
| 562 | 64 | four screen styles, 16 bytes each (two switches, three colours, text scale, image) |
| 626 | 36 | seven switches, nine one-byte values (language, brightness, rotation, counts, screen defaults), ten 16-bit timing/budget values |
| 662 | 1 | crest image |
| 663 | 1 + n | slides: count (0–5), then one byte each (0 = crest, else library id) |
| next | 1 + 236 n | competitions: count (0–3); each two switches, primary and fallback source (84 bytes each: provider, competition, season, club), competition name (33), club name (33) |
| next | 1 + 69 n | fixture pairs: count (0–8); each two switches, competition index, primary ID (33), secondary ID (33) |

A missing tail at a list or field boundary retains initial values; a stored
count of zero replaces a preset or initial list by an empty one. Ending inside
a field fails; a count above the list capacity rejects the record; an unknown
same-format tail is ignored within the reader limit.

The decoder also accepts the older formats and converts them in RAM:

- **Format 2** (1720 bytes) stored three routes, three fallbacks and eight
  fixture mappings as fixed slots with an enabled flag each. A complete route
  becomes a competition in slot order (a disabled one stays as a paused
  competition), an empty or incomplete slot disappears, the fallback of the
  route becomes its competition's fallback, and a mapping is renumbered to its
  competition or dropped with its route or when an ID is missing.
- **Format 1** (core only, up to the old 1024-byte limit) imports its known
  core through byte 334; unknown legacy tails are ignored after the CRC check.
  The 238- and 335-byte core layouts therefore remain readable, and newly added
  fields keep initial values.

Loading never rewrites NVS. The next explicit save, after any OTA trial,
writes format 3. The old firmware cannot read a newer record and uses initial
values after a manual USB downgrade; no reverse migration or automatic
downgrade write is performed.

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

Routine firmware OTA preserves LittleFS. The browser OTA gate accepts an
image whose declared settings format is equal to or newer than the running one
and refuses an older one, because every firmware imports all older formats and
an older image could not read the stored record. The first format-3 image is
installed by USB, since the previous firmware's gate demands an equal format.
A USB downgrade to older firmware selects its initial values once format 3 has
been saved. Trial firmware refuses settings and image writes (and the removal
of legacy image files) until local boot acceptance, so automatic rollback can
still read the untouched record and files. Rollback does not undo shared
storage after an accepted image has subsequently saved new settings.

The first format-3 boot removes the earlier fixed-name image files (crest,
five slides, four backgrounds) once, outside a trial; they are not migrated
and must be uploaded again into the library.

## Resources and acceptance

Compiler sizes (host compiler; the same method gave the documented 1736 bytes
for the format-2 model): Model 1944 bytes, longest encoded record 1931 bytes.
The stored model and network's applied copy are fixed RAM; every
`settings::current()` copy puts about 1.9 KB on the caller's stack (208 bytes
more than before). Save allocates the longest record size (1931 bytes)
temporarily; load allocates at most 2048 bytes. These costs are additional to
NVS page/index heap. A 1931-byte blob uses roughly 63 NVS entries, about 126
while replacing it; these are layout estimates, not measured full-storage
acceptance. The partition remains 20 KB. A full model serializes to 3,906 bytes
of JSON (4,358 with secrets, host measurement), within the 8,192-byte API body
limit.

The earlier 238-byte layout passed C6 store selectors on 2026-10-09 and
30 reset-button interruptions; supply removal remains untested. The host
tests (`pio test -e native -f test_settings`) cover the format-3 layout, the
longest record, the conversion of formats 1 and 2, list limits and the image
reference cleanup. They do not certify device behaviour: the migration of a
real format-2 record, full/corrupt storage, real power loss, resets and OTA
rollback still require tests on the device. Current store selectors remain
available in `app_config.h`, normally `kNone`.
