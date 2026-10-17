# Configuration

Implementation state: 0.1.0-dev, 2026-10-17 (settings format 11, competition record format 4, screen-policy record format 1). The current settings groups are
connected to the device services and offline Web UI. The current C6 build
compiles; new fields, persistence interruptions and runtime controls still
require acceptance. Historical core-service tests do not certify this extension.

The [planned screen-system redesign](UI.md#planned-screen-system-redesign)
replaces situation-to-single-screen defaults with configurable timed sequences,
separates crest from slideshow, adds competition-specific rules and a picture
frame mode, and replaces the whole-matchday own-game switch. Those are confirmed
behaviors being integrated. The existing settings service now owns a dedicated
complete screen-policy snapshot and record, avoiding rule copies in network and
provider settings. The active controller uses this policy. Replaced display fields have been removed from the general record and API.
The screen policy is their single owner.

The service loads/saves the policy, REST/Web exposes it, and the controller
uses its own coherent snapshot. Its five-phase/six-type
snapshot is 964 B on C6 and its explicit version-1 record is 912 B. During this development
redesign settings may restart from defaults/presets; compatibility migration is
reconsidered after alpha/beta. Existing image files need not be deleted. No
agent inspection of the secrets file is required for firmware presets.

## Three sources, one place each

| What | Where | In Git |
|---|---|---|
| Hardware facts | `boards/`, `displays/`, `targets/` | yes |
| Software defaults, limits and enums | `include/app_config.h` | yes |
| Personal bootstrap presets | `include/secrets.h` | no |
| Runtime configuration | NVS namespace `settings`, key `record` | device only |
| Competitions | NVS namespace `settings`, key `competitions` | device only |
| Screen policy | NVS namespace `settings`, key `screen_policy` | device only |
| Owner's team short names | LittleFS `short_names.bin` | device only, in the export |

The owner requested a maintained club-name catalogue beside app_config.h and
the secrets file in include/. Proposed source: include/team_names.json, covering
Bundesliga,2.Bundesliga,3.Liga and German Regionalligen, with German/English/
short names and optional provider IDs. The selected JSON shape uses direct
de/en/short fields per club, with provider IDs grouped under ids;
the file and new mapping/import format are not implemented. Missing catalogue
values must retain usable provider fallbacks. This must extend the existing
names mechanism, with a separately bounded device memory/storage design.

The template `include/secrets.h.example` is tracked. A stored record overrides
bootstrap presets; omitted appended fields use their initial preset/default.
An invalid preset is skipped and its field name is logged without its value.
Settings are typed fixed storage in `components/settings/`; the NVS adapter
is `components/core/settings_store.cpp`. The competitions
are a separate store with its own record and REST endpoint
(`/api/v1/competitions`, see [API.md](API.md)); the settings JSON does not
carry them. Successful save/reset of either store increments the settings
generation and posts a short change notification after publishing the model
and its version together under the existing settings mutex. Readers can copy
both in one operation; no second model is retained. A second counter, the
provider-data generation, increments on saves only when a value provider data
depends on changes: a competition's id, position, active flag or its source
(provider, competition, season, club), either provider key, the ESPN opt-in or demo mode. Only that counter discards
fetched data and interrupts a running provider request: the data of the
changed, removed or paused competitions, of the competitions that use a
provider whose key or ESPN opt-in changed, or all data after a change of
demo mode. Reset additionally forces data invalidation even if the initial
provider values already match; if they change, both invalidations advance
the counter. These counters are version markers, not save counts.
Display names, screen-policy rules and every display,
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
`SECRET_DEFAULT_COMPETITION`, `SECRET_DEFAULT_TEAM_ID` and
`SECRET_DEFAULT_SEASON`. Provider values are `openligadb`, `api-football`,
`espn` or `football-data`. ESPN additionally requires explicit runtime opt-in.
A settings or competitions change through the REST API is refused
(`conflict` at `competitions[i].primary.provider`) while the source of an
active competition uses ESPN without the opt-in, or API-Football/football-data.org without a
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
| Competition screen rules | inherited per phase | enable, automatic participation and visit time live in the dedicated policy; each override field can independently inherit |
| Competition source | OpenLigaDB | one source per competition: provider, competition ID and club ID up to 32 bytes of any text without control characters and without a space at either end (requests percent-encode them); season empty (current season) or up to 16 decimal digits for OpenLigaDB, four digits for other providers |
| Display names | empty | competition and club name stored when picked from a provider list, up to 32 bytes, no control characters |
| ESPN / demo / browser diagnostics | off | explicit switches |
| Sporting types / image types / backgrounds | enabled / disabled / on | six separate type styles; an empty automatic sequence is allowed |
| Background / text / accent | `#101820` / `#ffffff` / `#49cba0` | RGB colours per screen |
| Text scale | 100% | 70–130%, selects bounded compiled font roles |
| Common sequence / automatic default per phase | Crest, Images, Single, Matchday, Conference, Table / Own: Single, Other: Conference, Matchday: Matchday, Idle and Break: Table | enable and automatic flags are independent per phase; shared sporting rules may be overridden per competition |
| Conference mode | All pages | All pages uses complete chronological members; One page ranks current activity; explicit manual paging always reaches all members |
| Pin own match in the conference | on | on: the own match is the first row of every conference page while it is shown; off: a normal row in the accent colour |
| Matchday shown | Current matchday | Current matchday (the running or next matchday; a finished one stays until the end of the following local day) or Next matchday (moves on at midnight after the last kickoff; a competition without a stored upcoming matchday is skipped by the automatic cycle); one setting for all competitions |
| Input assignment | input 1 Mandatory, input 2 Up, input 3 Down (two inputs: Mandatory, Scroll) | per profile input Off, Mandatory, Up, Down or Scroll; exactly one of the device's inputs is Mandatory; eight stored slots, slots beyond the profile's inputs are ignored |
| Manual return / scroll-direction reset | 60 / 20 seconds | 0–4000; manual return lives in the policy; 0 disables the corresponding timeout; direction reset applies to Scroll inputs |
| Scroll repetition | 250 ms | 100–2000 ms |
| Conference and Matchday rows / table rows | 5 / 5, clamped to profile (240 px round: 4 / 5) | shared match-page maximum 2–5 (240 round 2–4); table 1–9 (240 round 1–7). Actual content can be smaller. Pin counts toward capacity; page balancing preserves page count |
| Sporting visit time | Single/Conference/Matchday 30 s, Table 60 s | 15–43,200 s; divided across pages, effective total at least 5 s per page; no independent page clocks |
| Images / Crest | Images visit 60 s, Crest visit 15 s; images initially disabled | image time 15 s (3–3,600); up to five library images, crest separate; enabled Images visit must be an exact multiple of image time |
| Image library | empty | up to ten uploaded JPEGs (ids 1–10); screen backgrounds and slides refer to them. Selected missing files keep their ids reserved for explicit restoration; new uploads skip saved references and the current browser's draft references. Id 0 is the stock image of the role (crest or background): its replacement when one exists, else the factory image. The crest is not a setting; it is always the crest role. Replacements count towards the shared 320 KB image budget but not towards the ten library ids |
| Brightness / night brightness | 100% / 10% | 0–100%; hardware dimming requires a backlight pin |
| Rotation | 0 | 0–3 quarter turns |
| Language | German | German or English |
| Night mode / window | on / 23:00–07:00 | local civil time; equal endpoints mean empty; requires valid clock |
| IP badge | 60 seconds, top | 0–4000 seconds, 0 permanent; setup AP permanent by default |
| Own and other display windows | each before/after 30 min | four independent 0–180-minute policy values; no whole-matchday own-retention switch |
| Daily request cap per provider | 0 / 100 / 0 / 0 requests | 0–65535 per provider; 0 = no daily cap (no counter, no storage write); a cap counts every attempted request through durable reservations and resets at 00:00 UTC |
| Window limit per provider | 60 per 60 s / off / off / 10 per 60 s | requests 0–65535 per window of 0–43200 seconds; both values 0 or both set; 0 = no window limit |
| Interval per data type and provider | fixtures 8 h, table 4 h, live 60 s, live table 10 min, events 10 min | 10 s – 7 days each; how often the provider is asked for that data type |
| Global limit over all providers | off (0 / 0 per 0 s) | daily cap and window limit as above, counted over every provider |
| Console status | on, 30 seconds | existing stored interval limits; browser stream is separate |

Stable type identities are Single0, Conference1, Table2, Crest3, Matchday4,
Images5. These identities are independent of the editable sequence order. The
schema exposes the six policy type names and bounds; old `screens`, screen
masks/defaults, `table_page_s` and slideshow fields in general settings are
rejected. Appearance, pinning, row counts and timing belong to screen policy.

Provider indices are OpenLigaDB 0, API-Football 1, ESPN 2, football-data 3.
Each provider has a request plan (JSON `providers`, a list in that provider
order): `daily_requests`, `window_requests`, `window_s` and `intervals`
(`fixtures`, `table`, `live`, `live_table`, `events`, seconds). A
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
slideshow dots, `kGoalFlashS` (120 s), how long a Konferenz row stays
highlighted after a goal, `kStatusPillS` (5 s), how long the status pill is
shown after a status change, and `kStatusPillOpa` (204 of 255), the opacity
of its background.
Request timing constants (compile time, `app_config.h`): `kRequestGapMs`
(2 s between any two request starts), `kLiveLeadMinutes` (30, live data is
requested from this long before kickoff), `kExpectedMatchMinutes` (110,
expected end of a match without final whistle or provider-reported running
phase), `kUnknownMatchEndMinutes` (240, the latest end of a match reported
running), `kProviderErrorRetryS` (60 s first retry after a failed request,
doubling per failure), `kProviderErrorRetryMaxS` (900 s: the doubling stops
at the larger of this and the data type's interval, so live data back off
to 15 minutes), `kProviderRateRetryS` (300 s pause
of a provider after "too many requests"), `kLimitWindowBuckets` (32 time
buckets per window limit in the request scheduler; 2 bytes each),
`kStoreMaintenanceS` (60 s, how often passed matchdays are dropped and
OpenLigaDB phases re-estimated, also the longest sleep of the provider
worker) and `kDataStaleS` (900 s: a shown match in its live window whose
competition's last successful match request is older marks the data
stale) and `kUnmaintainedAfterS` (24 h: a match that kicked off this long
ago, is neither finished, postponed, cancelled nor abandoned and has no
result marks its provider as not maintaining the competition).
Network/time recovery constants are `kWifiRadioRetryMs` (10,000 ms for
failed local network initialization phases, radio/AP reconfiguration,
initial/runtime hostname acknowledgement and SNTP initialization) and
`kTimeZoneRetryMs` (10,000 ms after a failed time-zone application). Confirmed
network phases/resources remain active while only failed work is repeated.
These compile-time delays do not require Internet/SNTP for local readiness.

`kImageMaintenanceRetryMs` (10,000 ms) delays another startup-maintenance
attempt after legacy-image removal fails. Maintenance no longer removes
references just because a file is missing. `kImageSourceRetryMs` (10,000 ms)
retries the desired image after file/source/decode failure; a new file
revision is checked immediately. `kImageValidationTimeoutMs` (1,000 ms)
bounds each admitted full-file/fallback-resolution pass, reusing the existing
JPEG workspace. A longer candidate chain resumes in a later ordinary app
poll. Waiting for exclusive-operation admission is nonblocking and does not
hold that admission during the delay. None is a user setting.

Lists never contain empty entries: an entry exists or the list is shorter.
Crest displays its dedicated role; Images uses the policy library list. Neither
requires a sporting background image. Missing/damaged referenced files retain their desired
id and slideshow position and use the appropriate role/factory fallback.
Explicit manual deletion clears only that id; confirmed repair/reset retains
its own destructive semantics. Image errors never automatically save settings.

## Record format and consistent saves

The on-demand screenshot has compile-time limits in `app_config.h`:
`kScreenshotRows = 20` and `kScreenshotDeadlineMs = 5000`. The port also
limits capture to half the configured watchdog window, keeping shorter
watchdog profiles safe. These are not saved settings; no record format,
NVS entry, image quota or LittleFS allocation changes. The active 240×240
C6 requests one temporary 9,600 B internal RGB565 strip plus allocator
metadata, released after success or cancellation. See [API.md](API.md) for
the wire protocol and [WEB_UI.md](WEB_UI.md) for access and download behavior.

Current general format is **11**, explicitly little-endian, not a structure
memory dump. Its complete fixed record is **700 bytes**. Text occupies a length
byte plus fixed capacity. The reader remains bounded to2048B.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | format u16, payload length u16, CRC32 over header first4B and payload |
| 8 | 227 | WiFi SSID, station/AP passwords, hostname, each length-prefixed |
| 234 | 4 | antenna, console flag, console interval u16 |
| 238 | 97 | timezone label and NTP server |
| 335 | 227 | admin/API-Football/football-data secrets |
| 562 | 20 | six flags, language/day/night brightness/rotation, five u16 input/night/badge times |
| 582 | 8 | input roles |
| 590 | 104 | four request plans: three u16 limits plus five u32 intervals each |
| 694 | 6 | global three u16 request limits |

Competition format **4** uses the same8B header, then next-id u8,
next-incarnation u32 and count u8. Each entry is156B: id1, incarnation4,
active1, source84, competition name33 and club name33. Maximum eight entries
produce **1262B**, empty14B. The reader remains bounded to4096B. There is no
screen mask. Policy bindings use both id and incarnation.

Older general formats1–10 and competition formats1–3 select initial values
and report `outdated`; development settings may reset. Unknown future formats,
length/CRC corruption or invalid values are rejected. Decoding never rewrites
persistent state. Images stay separate. A missing field-boundary tail may keep
initial values; a partial field fails. Records remain stored until an explicit
save/reset. An unreadable competition identity history blocks unsafe saves.

The screen-policy record is independently versioned and CRC-protected. It has
an eight-byte header and 904-byte payload (912 bytes total). A successful save
publishes one complete policy and advances only the UI/settings generation,
so it does not cancel an admitted provider response. Saving a stale generation
is rejected. Competition overrides bind both id and incarnation: edit/reorder
keeps them; deletion and id reuse cannot transfer them to another entry.
Unmatched bindings are pruned in RAM, without an automatic write to a third
record. A fallback preset does not establish a stored identity: save the
competition record before persisting bound overrides. Trial boot derives these
values in RAM and does not migrate or rewrite the older records.

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
and refuses an older one, because an older image could not read the stored
record. Format-9 firmware is accepted by browser OTA from format-3 to format-8
firmware, but it does not read the older settings and competitions: they are
reported as outdated and must be set up again (or imported from a backup). (Historical: the first
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
(about 3 KB); a full short-name table 1,548 bytes including its 12-byte header. Routine OTA keeps the
files; the first boot of an update writes none.

## Resources and acceptance

Current C6 compiler ABI: Model704B (formerly832), Competitions1260B
(formerly1292), Policy964B. General and competition records are700B and at most
1262B; the policy record is912B. Settings owns one policy snapshot and UI owns
one coherent snapshot; effective competition routes are borrowed while the
repository mutex is held. There is no UI competition-store copy.

General save allocates700B, competition save up to1262B, policy save912B, plus
NVS internals. REST input DOM is released before encoding policy. NVS replacement
space, power interruption, fragmentation and the20KiB partition remain device
acceptance; payload sizes do not prove these reserves. Reset/migration behavior
is separate from preserving existing image files.

The strict complete policy request is at most8121B, within8192B HTTP input.
The JSON allocator cap stays24576B. Policy GET/PUT and the full provider response/completion lifecycle now share
the existing heavy-operation token; discovery also acquires it before allocating
state. Busy requests leave the draft/save unchanged and can be retried.
Canonical API logical growth is5545B (6536B with conservative moving-realloc
accounting), disjoint from2016B event staging or532B discovery plus816B fence.
Actual ROM allocator overhead, fragmentation and runtime reserves remain device
acceptance, not certified by host pointer sizes or one fixture.

The earlier 238-byte layout passed C6 store selectors on 2026-10-09 and
30 reset-button interruptions; supply removal remains untested. The host
tests (`pio test -e native -f test_settings`) cover the current layout and
the competition record layout, the rejection of older formats as outdated,
id assignment, the request plans, the input-role rules, the longest records,
list limits and the image reference cleanup. They do not certify device
behaviour: the outdated report after an update from a device with an older
record, full/corrupt storage, real power loss, resets and OTA
rollback still require tests on the device. Current store selectors remain
available in `app_config.h`, normally `kNone`.


## Image-reference cleanup across records

An explicit library deletion cleans only that image id from both the general
record and the screen policy before deleting the file. Repair cleans every
missing library reference after formatting. Disabled styles still reserve their
desired images; selected slide duplicates are removed completely and surviving
order is preserved. Each successfully written record is published immediately
and advances the UI generation. The existing writer remains held until explicit
unlink completes, so a concurrent save cannot reintroduce the reference in
between. The two NVS records and filesystem operation are not one transaction;
partial completion is reported and the browser keeps unconfirmed edits.
