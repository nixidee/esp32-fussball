# Configuration

Implementation state: 0.1.0-dev, 2026-10-14 (settings format 8, competition record format 1). All planned settings groups are
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
| Competitions and fixture pairs | NVS namespace `settings`, key `competitions` | device only |
| Owner's team short names | LittleFS `short_names.bin` | device only, in the export |

The template `include/secrets.h.example` is tracked. A stored record overrides
bootstrap presets; omitted appended fields use their initial preset/default.
An invalid preset is skipped and its field name is logged without its value.
Settings are typed fixed storage in `components/settings/`; the NVS adapter
is `components/core/settings_store.cpp`. The competitions and fixture pairs
are a separate store with its own record and REST endpoint
(`/api/v1/competitions`, see [API.md](API.md)); the settings JSON does not
carry them. Successful save/reset of either store increments the settings
generation and posts a short change notification. A second counter, the
provider-data generation, increments only when a value provider data depends
on changes: a competition's id, position, active flags or its primary or
alternate source (provider, competition, season, club), the fixture pairs,
either provider key, the ESPN opt-in or demo mode. Only that counter discards
fetched data and interrupts a running provider request: the data of the
changed, removed or paused competitions, of the competitions that use a
provider whose key or ESPN opt-in changed, or all data after a change of
demo mode.
Display names, the screens a competition is used for and every display,
network or time setting keep the shown data and request nothing. A changed
match window, request limit or interval applies with the next scheduling
decision: a job whose due time under the new value has passed runs then, the
others keep their due times. Only the Web UI's refresh action makes every job
due at once.

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
A settings or competitions change through the REST API is refused
(`conflict` at `competitions[i].primary.provider` or `.fallback.provider`)
while an active competition's primary source, or its active fallback source,
uses ESPN without the opt-in, or API-Football/football-data.org without a
key. The check runs against the other store's saved state, so switching one
competition from one keyed provider to another is done in two saves: first
enable the new provider, then change the competition. A stored
record from older firmware that breaks this rule still loads unchanged; the
affected source is then not requested until the key or opt-in is saved.
Competition/team IDs always belong to that provider. A season is a provider
season, rather than a year inferred from the device's calendar.

Former `SECRET_WIFI_SSID_1`/`SECRET_WIFI_PASSWORD_1` and `SECRET_TIMEZONE`
names deliberately stop the build. The time-zone preset is a supported
location label, such as `Europe/Berlin`, rather than a POSIX rule.

## Defaults and bounds

The Web UI reads `GET /api/v1/schema`; it does not maintain another defaults
table. Every rejected value is answered with the name of its field and the
reason (see [API.md](API.md)). Partial settings requests retain omitted fields, including secrets.
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
| Competitions | empty list; a preset competition (with or without club) creates one entry | ordered list of up to eight entries, each active or paused, each with a stable id 1–255 assigned by the device; the order is the display order: Matchday and Table show the competitions one after another in this order |
| Screens per competition | all four | the competition appears on Own match, Konferenz, Matchday and Table (`screens` switches, JSON `own_match`, `conference`, `matchday`, `table`) |
| Competition source | OpenLigaDB | provider, competition ID and club ID up to 32 bytes of any text without control characters and without a space at either end (requests percent-encode them); season empty (current season) or up to 16 decimal digits for OpenLigaDB, four digits for other providers |
| Fallback (per competition) | off | a second source of another provider, active or paused; a paused fallback may stay incomplete |
| Display names | empty | competition and club name stored when picked from a provider list, up to 32 bytes, no control characters |
| Fixture pairs | empty list | up to eight explicitly verified pairs, each referring to a competition by its id; the Web UI offers pairs only for a saved active competition with a club and an active fallback |
| ESPN / demo / browser diagnostics | off | explicit switches |
| Screens / backgrounds | all enabled / on | five styles; at least one screen enabled |
| Background / text / accent | `#101820` / `#ffffff` / `#49cba0` | RGB colours per screen |
| Text scale | 100% | 70–130%, selects bounded compiled font roles |
| Defaults outside / matchday / own game | Table / Conference / Own match | enabled available screen (any of the five); while other matches run, Own match is not selectable (the own club is not playing then) |
| Conference mode | All pages | All pages (every member in kick-off order, pages change every 8 s) or One page (running matches by latest goal, goalless ones by kick-off, then finished, then not started) |
| Pin own match in the conference | on | on: the own match is the first row of every conference page while it is shown; off: a normal row in the accent colour |
| Show own match for the whole matchday | off | on: from kick-off the own match stays in the conference while matches of the same matchday (same round of the same competition, otherwise same date) are shown there |
| Input assignment | input 1 Mandatory, input 2 Up, input 3 Down (two inputs: Mandatory, Scroll) | per profile input Off, Mandatory, Up, Down or Scroll; exactly one of the device's inputs is Mandatory; eight stored slots, slots beyond the profile's inputs are ignored |
| Manual return / scroll-direction reset | 60 / 20 seconds | 0–4000; 0 disables the corresponding timeout; the direction reset matters only while an input is assigned Scroll |
| Scroll repetition | 250 ms | 100–2000 ms |
| Visible conference / table rows | 5 / 5 (240 px round: 4 / 5) | 1–5 / 1–9 (240 px round: 1–4 / 1–7); the conference value is also the number of matches per Matchday page; the display profile sets the upper limits, the schema reports them, the API rejects more, stored values above them are clamped when loaded; all stored table rows remain reachable |
| Table step | 8 seconds | 3–600 seconds; time until the Table screen shows the next page of a table without an own club, or (with an own club, or after the last page) the next competition's table |
| Slideshow | 15 seconds, no slides | 3–3600 seconds; up to five slides in play order, each the crest or a library image |
| Image library | empty | up to ten uploaded JPEGs (ids 1–10); screen backgrounds and slides refer to them; id 0 is the stock image of the role (crest or background): its replacement when one exists, else the factory image. The crest is not a setting; it is always the crest role. Replacements count towards the shared 320 KB image budget but not towards the ten library ids |
| Brightness / night brightness | 100% / 10% | 0–100%; hardware dimming requires a backlight pin |
| Rotation | 0 | 0–3 quarter turns |
| Language | German | German or English |
| Night mode / window | on / 23:00–07:00 | local civil time; equal endpoints mean empty; requires valid clock |
| IP badge | 60 seconds, top | 0–4000 seconds, 0 permanent; setup AP permanent by default |
| Match window before / after | 30 / 30 minutes | 0–180 minutes |
| Daily request cap per provider | 0 / 100 / 0 / 0 requests | 0–65535 per provider; 0 = no daily cap (no counter, no storage write); a cap counts every attempted request through durable reservations and resets at 00:00 UTC |
| Window limit per provider | 60 per 60 s / off / off / 10 per 60 s | requests 0–65535 per window of 0–43200 seconds; both values 0 or both set; 0 = no window limit |
| Interval per data type and provider | core 24 h, fixtures 8 h, table 4 h, live 60 s, live table 10 min, events 10 min | 10 s – 7 days each; how often the provider is asked for that data type |
| Global limit over all providers | off (0 / 0 per 0 s) | daily cap and window limit as above, counted over every provider |
| Console status | on, 30 seconds | existing stored interval limits; browser stream is separate |

Screen indices are Own match 0, Conference 1, Table 2, Crest/slideshow 3,
Matchday 4 (the stored order). The display order (Next/Previous, Web UI) is
Own match, Conference, Matchday, Table, Crest/slideshow; the schema reports
it as `screen_order`.
Provider indices are OpenLigaDB 0, API-Football 1, ESPN 2, football-data 3.
Each provider has a request plan (JSON `providers`, a list in that provider
order): `daily_requests`, `window_requests`, `window_s` and `intervals`
(`core`, `fixtures`, `table`, `live`, `live_table`, `events`, seconds). A
shorter list in a REST save or import changes only its first plans, and an
absent key keeps its value. `global_limit` has the same three limit keys and
counts every provider together. The limits apply to every request start of
that provider, including selection lists. The request scheduler counts a
window limit exactly (never more than `window_requests` in any `window_s`
seconds), every two requests of the device keep a 2-second gap, the
provider's and the global daily cap are enforced, and the intervals set the
due time of each data type. When a daily cap cannot cover the requests
expected until 00:00 UTC, data types are stretched or skipped (live table
first, the own match last). Defaults follow the providers' published limits (checked 2026-10-13):
API-Football's free plan allows 100 requests per day, football-data.org's
free tier 10 per minute, OpenLigaDB 60 per minute per IP address without a
daily quota; ESPN publishes no limit, so its plan has no limit.
An empty season means the provider's current season at every provider; the
device resolves it at runtime (see the data provider documentation). For
OpenLigaDB the competition list
uses the season named after the current year from `kSeasonStartMonth`
(July) on, before that the previous year's, and match requests let
OpenLigaDB report the current season. A season typed in the setup wizard's
season field is stored with the competition; picking from the current list
leaves it empty. WiFi modem sleep is fixed off by `kWifiPowerSave`.
On the current C6 without a backlight pin, night mode switches to the night
background (`kNightBackground`), shows images at 20 % opacity and mixes
every text, pill and rim colour towards the night background; night
brightness sets that mix as an amount of light (colour value =
level^(1/2.2), so 10 % keeps about 35 % of each colour). A profile with a
backlight pin keeps the colours and dims the backlight instead. A changed
value is not evidence of the physical brightness on the device.
Display design constants (compile time, `app_config.h`): font role sizes per
size class (`kTextRolePx`, caption/body/title/score; size classes split at
`kSizeClassSmallMax` 260 and `kSizeClassMediumMax` 400 px shorter side),
the semantic colours for warnings, errors, neutral states, the IP chip and
slideshow dots, and `kGoalFlashS` (120 s), how long a Konferenz row stays
highlighted after a goal.
Request timing constants (compile time, `app_config.h`): `kRequestGapMs`
(2 s between any two request starts), `kLiveLeadMinutes` (30, live data is
requested from this long before kickoff), `kExpectedMatchMinutes` (110,
expected end of a match without final whistle or provider-reported running
phase), `kUnknownMatchEndMinutes` (240, the latest end of a match reported
running), `kProviderErrorRetryS` (60 s first retry after a failed request,
doubling up to the data type's interval), `kProviderRateRetryS` (300 s pause
of a provider after "too many requests"), `kLimitWindowBuckets` (32 time
buckets per window limit in the request scheduler; 2 bytes each),
`kStoreMaintenanceS` (60 s, how often passed matchdays are dropped and
OpenLigaDB phases re-estimated, also the longest sleep of the provider
worker) and `kDataStaleS` (900 s: a shown match in its live window whose
competition's last successful match request is older marks the data
stale).
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

Current format is **8**, with explicit little-endian encoding, not a structure
memory dump. The record has a variable length: the slide list stores only its
entries. The longest possible record (five slides, every text at capacity)
is **821 bytes**, pinned by a host test; a record without slides is 816 bytes.
The reader accepts at most 2048 bytes (`kSettingsMaxRecordBytes`). Header:
16-bit format, 16-bit payload length and IEEE CRC32 over the first four bytes
plus payload. The competitions and fixture pairs are no longer part of this
record; they have their own record (below).

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
| 626 | 34 | seven switches, nine one-byte values (language, brightness, rotation, counts, screen defaults), nine 16-bit timing values |
| 660 | 1 + n | slides: count (0–5), then one byte each (0 = crest, else library id) |
| next | 1 | pin own match in the conference (0/1) |
| next | 1 | conference mode (0 All pages, 1 One page) |
| next | 1 | show own match for the whole matchday (0/1) |
| next | 8 | input roles, one byte per input slot (0 Off, 1 Mandatory, 2 Up, 3 Down, 4 Scroll) |
| next | 4 × 30 | request plan per provider: daily cap, window requests, window seconds (16 bits each), then the six intervals in seconds (32 bits each: core, fixtures, table, live, live table, events) |
| next | 6 | global limit: daily cap, window requests, window seconds (16 bits each) |
| next | 16 | Matchday screen style (same layout as the four styles at 562) |
| next | 2 | table step in seconds (16 bits) |

The competition record (key `competitions`, its own format **1**, same
header) holds:

| Size | Field |
|---|---|
| 1 | next id to assign (1–255, wraps; a free id is searched) |
| 1 + 238 n | competitions: count (0–8); each id (1 byte), two switches, primary and fallback source (84 bytes each: provider, competition, season, club), competition name (33), club name (33), screens (1 byte, one bit per screen) |
| 1 + 69 n | fixture pairs: count (0–8); each two switches, competition id, primary ID (33), secondary ID (33) |

The longest competition record is **2467 bytes**, an empty one 11 bytes; the
reader accepts at most 4096 bytes (`kCompetitionsMaxRecordBytes`). A damaged
or rejected competition record selects the initial competitions (the preset
competition, if any) and is overwritten by the next competitions save; status
reports it as `competitions_source`.

A missing tail at a list or field boundary retains initial values; a stored
count of zero replaces a preset or initial list by an empty one. Ending inside
a field fails; a count above the list capacity rejects the record; an unknown
same-format tail is ignored within the reader limit.

The decoder also accepts the older formats and converts them in RAM. Formats
2 to 7 hold the competitions in the settings record (format 3 to 7: after the
slides, up to three competitions without id and screens, then fixture pairs
that refer to a competition by list position). They become competitions with
the ids 1, 2, 3 in list order, with all four screen switches on; pairs are renumbered
to these ids. Formats 4 to 7 add, in this order, the pin flag, the format-5
fields and (format 7) the request limits after the lists.

- **Format 7** stores a daily cap and a minimum request spacing per provider
  (8 bytes each). The daily caps are kept; the spacings are dropped and the
  window limits, intervals and global limit keep their initial values.

- **Format 6** stores the API-Football daily budget as a tenth 16-bit value
  in the scalars (slides at 662) and has no request limits at the end. That
  value becomes API-Football's daily cap; everything else of the plans keeps
  its initial value. A format-6 record with bytes after the input
  roles ignores them.
- **Format 5** has the format-6 layout with one more byte, the crest image
  id, between the scalar values and the slides. The crest is no longer a setting: a library id
  there is handed to the image service, which copies that image once as the
  replacement of the crest role (outside an OTA trial; retried while the
  display is busy or an upload runs) and then saves the settings in the
  current format. The library image stays.
  If the image no longer exists, or the copy fails for lack of storage, the
  crest role keeps its factory image and the record is still saved in
  the current format; the image stays in the library. A crest id outside the library
  range is ignored instead of rejecting the record. Formats 3 and 4 carry the
  same byte and are handled the same way.
- **Format 4** has the same layout without the ten format-5 bytes; conference
  mode, whole-matchday switch and input roles keep their initial values (All
  pages, off, the profile's default assignment).
- **Format 3** has the format-4 layout without the final pin flag; the flag
  keeps its initial value (on).

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

A stored matchday default of Own match (allowed up to format 3) is read as
Conference in every format; everything else in the record is kept. A backup
file with that value is converted the same way by the Web UI before import.

Loading never rewrites NVS. A readable format 2 to 7 settings record was
written last by older firmware, so its competitions replace any competition
record at boot (for example after a downgrade, changes on the old firmware
and a new update). The next explicit save of either store, after any OTA
trial, first writes the competition record and then the settings record in
format 8; from then on the competition record is used. Interrupted between
the two writes, the next boot takes the unchanged competitions from the old
record again (the crest migration above saves once by itself). The old firmware cannot read a newer record and uses initial
values after a manual USB downgrade; no reverse migration or automatic
downgrade write is performed.

A whole record is written and committed before RAM settings change. NVS
provides atomic blob replacement; interruption acceptance remains a separate
physical test. Loading checks size, length, CRC, format and value limits.
Damaged/unknown records select initial values and remain stored until save
or reset. An unreadable record prevents saving data that was never checked.
The REST status reports which of these cases applies (`settings_source`);
the Web UI shows it as a warning on the overview and asks for confirmation
before the first save overwrites a rejected or damaged record.

The established NVS startup recovery erases the entire NVS partition only
for `NO_FREE_PAGES` or `NEW_VERSION_FOUND`, with an error log. That also
clears radio calibration and the request counters. Other NVS errors
retain RAM defaults and refuse persistent saves.

## Reset and firmware updates

System reset always clears the `settings` namespace (settings and
competition record), the request counters, the kept football data and the
owner's team short names, and restarts. User images
are retained unless the explicit image-deletion option is selected; that
option deletes the library and the crest and background replacements, so both
roles show their factory images again. The separate
full image-storage repair option formats LittleFS, including stock files.
It requires explicit confirmation and never follows automatically from a
mount fault. Stock files then need initial USB `uploadfs`; custom files can
be uploaded through the Web UI.

There is no input gesture that resets settings; the reset is available in
the Web UI only.

Request counters live in their own NVS namespace (`provider_budget`), one
12-byte record per provider with a daily cap (magic, UTC day, reserved
requests) and one more (key `global`) while the global daily cap is set. A
provider with cap 0 has no counter and causes no write. Before
network I/O a counter reserves blocks of up to eight requests; unused
reserved requests are lost on reboot, and a backward clock correction never
reopens a day. A later UTC day starts a new count. A record that cannot be
stored refuses that provider's request rather than bypassing the cap. A
damaged or unreadable record blocks only its own provider for the current
day; the next UTC day writes a new record. The single 24-byte record of
earlier firmware (key `credits`) is read once for API-Football while that
provider has no record of its own and removed when the first new record is
stored. The settings reset in the Web UI also erases every counter, so a
damaged counter can always be cleared without a serial tool. This is a local
limit, not a guarantee against other devices sharing the provider quota or
IP address.

Routine firmware OTA preserves LittleFS. The browser OTA gate accepts an
image whose declared settings format is equal to or newer than the running one
and refuses an older one, because every firmware imports all older formats and
an older image could not read the stored record. Format-8 firmware is therefore
accepted by browser OTA from format-3 to format-7 firmware. (Historical: the first
format-3 image had to be installed by USB, because the format-2 firmware's
gate demanded an equal format.) A USB downgrade to older firmware selects its
initial values once a newer format has been saved. Trial firmware refuses
settings, competition and image writes (and the removal of legacy image
files) until local boot acceptance, and loading writes nothing, so automatic
rollback can still read the untouched record and files. Rollback does not undo shared
storage after an accepted image has subsequently saved new settings.

The first format-3 boot removes the earlier fixed-name image files (crest,
five slides, four backgrounds) once, outside a trial; they are not migrated
and must be uploaded again into the library.

## Kept football data

The football service keeps each active competition's data in a LittleFS
file `comp_<id>.bin` and the owner's team short names in
`short_names.bin` (format: [DATA_MODEL.md](DATA_MODEL.md#48-competition-files-and-short-names),
write rules: [DATA_FLOW.md](DATA_FLOW.md#kept-data)). They are not
settings and have no NVS record; the settings export carries the short
names (not the competition files), and an import replaces them. Constants in
`include/app_config.h`:

| Constant | Value | Meaning |
|---|---|---|
| `kCompetitionFileGapS` | 600 s | at least this long between two writes of one competition file |
| `kFootballFilesBytes` | 48 KiB | all football files together, counted in whole blocks |
| `kFsBlockBytes` | 4096 B | LittleFS block size used for that count |
| `kShortNamesMax` | 48 | short names over all competitions, 26 bytes each |

Images come first: a write also leaves free on the filesystem the part of
the image library quota (`kImageTotalBytes`, 320 KiB) that images do not use
yet, in whole blocks, plus the image reserve (`kImageReserveBytes`, 40 KiB).
Future images are counted as one block-rounded total, not per file, so an
image upload near the quota can still find a few blocks fewer than its own
check expects. An 18-team league with an own match takes one block
(about 3 KB); a full short-name table 1,552 bytes. Routine OTA keeps the
files; the first boot of an update writes none.

## Resources and acceptance

Compiler sizes (host compiler; the same method gave the documented 1968 bytes
for the format-7 model): Model **844 bytes**, competition store
(`Competitions`) **2459 bytes**; longest encoded records 821 and 2467 bytes.
Format 8 moved the competitions and pairs (about 1.2 KB) out of the model and
added the plans (126 bytes of record).
The stored model, the stored competitions and network's applied copy of the
model are fixed RAM. Every `settings::current()` copy puts about 0.8 KB on the
caller's stack; readers of the competitions copy one entry (about 240 bytes)
or allocate the whole store on the heap (the REST handlers). Booting decodes
a legacy record with a temporary store on the stack (about 3.3 KB in total).
Save allocates the longest record size (821 bytes, plus 2467 bytes for the
competition record) temporarily; load allocates the stored size, at most
2048 and 4096 bytes. These costs are additional to NVS page/index heap. The
two blobs use roughly 27 and 79 NVS entries (estimate: 32 bytes per entry plus
an index), about twice that for the record being replaced; these are layout
estimates, not measured full-storage acceptance. The partition remains 20 KB.
Settings JSON: format 8 replaces `"daily_requests":[…]` and
`"request_spacing_s":[…]` (60 bytes with the defaults) by `"providers":[…]`
(628 bytes with the defaults, at most 722) and `"global_limit":{…}` (69, at
most 81 bytes), all computed, and no longer carries the competitions and
pairs; the total of a full model was not measured again. The competitions JSON
of a full store stays within the 8,192-byte API body limit: 5,388 bytes with
every text at capacity, at most 7,446 bytes if every text character needs an
escape (computed). The
schema repeats the settings in its defaults and adds `"inputs":3,` (11
bytes), the number of inputs of the device.

The earlier 238-byte layout passed C6 store selectors on 2026-10-09 and
30 reset-button interruptions; supply removal remains untested. The host
tests (`pio test -e native -f test_settings`) cover the format-8 layout and
the competition record layout, the reading of format-3 to format-7 records
(including the crest hand-over, the conversion of the request limits and the
competitions with their ids), id assignment, the request plans, the
input-role rules, the longest records, the conversion of formats 1 and 2,
list limits and the image reference cleanup. The boot order of the two
records (a legacy record wins, nothing is written while loading) is not
covered by a host test. They do not certify device behaviour: the migration of a
real format-2 record, full/corrupt storage, real power loss, resets and OTA
rollback still require tests on the device. Current store selectors remain
available in `app_config.h`, normally `kNone`.
