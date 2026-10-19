# Configuration

Browser interaction budgets also live in `include/app_config.h`, in
`kWebClientPolicy`. The existing offline asset generator embeds that numeric
table before the browser sources; the schema publishes the same entries. It
covers HTTP/upload limits, bounded busy retries, selection/Demo/status polling,
restart/OTA observation, image input debounce and notification lifetimes. These
are interaction budgets, independent of provider request limits and the local
OTA health trial. Changing their owner does not change their existing values.

Implementation checkpoint: 2026-10-18. General settings, competitions and screen
policy now belong to one AppConfig and one current-format NVS record. Football
provider data is RAM-only. Adopted names remain separate; one common Apply references and activates the
exact immutable names version. Host checks do not establish physical
C6 runtime, OTA or power-interruption acceptance.

The display configuration has shared rules for five situations and six screen
types, one common type order, appearance and timing options. Each competition
owns its optional fieldwise display overrides and selected-club preferences.
There is no second identity-association matrix. Switching back to shared rules
keeps dormant overrides; an explicit `false` remains different from inheritance.
The UI receives a compact normalized programme for the current situation, with
only enabled entries and their effective duration/automatic flag. Disabled
entries remain stored in their competition, not in the active programme.

The global Policy is260B and its encoded payload216B. Competition preferences
add80B per configured competition, inside the same AppConfig. The current
aggregate record has a maximum2618B payload-plus-header. Development updates
start from defaults/presets; legacy migration is reconsidered after alpha.
Existing image files need not be deleted. Firmware presets require no agent
inspection of the secrets file.

## Three sources, one place each

| What | Where | In Git |
|---|---|---|
| Hardware facts | `boards/`, `displays/`, `targets/` | yes |
| Software defaults, limits and enums | `include/app_config.h` | yes |
| Personal bootstrap presets | `include/secrets.h` | no |
| General configuration, competitions and screen policy | one NVS `settings/app_config` record, plus exact adopted-names reference/revision/apply ID | device only |
| Adopted club names and provider/ID exceptions | LittleFS `names_a.bin` / `names_b.bin`, exact AppConfig reference | device only; complete collection export/import |

The maintained source catalogue [include/team_names.json](../include/team_names.json)
exists beside `app_config.h`: 147 clubs covering Bundesliga, second Bundesliga,
third division and all five German Regionalligen, in 47,451 source bytes. Its
version-1 JSON has direct `de`, `en` and `short` fields, a stable `key`, retained
`league` metadata and `aliases`. Provider IDs are arrays under `ids.espn` and
`ids.openligadb`; an unknown identity is `[]`. Preferred CSV names remain
unchanged. Of the preferred English texts, 71 are literally attested by the
checked ESPN responses; the other 76 remain unverified as ESPN English forms.
Provider spellings retained as aliases do not require changing those preferences.
See [DATA_MODEL.md](DATA_MODEL.md#49-club-name-source-catalogue) for the evidence
limits and distinction between name and identity verification.

The firmware embeds a deterministic, pointer-free read-only catalogue with
12,767 B of complete source payload, plus its fingerprint, stream code and
alignment. `GET /api/v1/team-names/preset` streams the original JSON values
through bounded buffers and returns `X-Team-Names-Source` with the source
SHA256. USB/OTA and rollback supply the source belonging to the running app.
There is no catalogue heap tree, LittleFS/NVS source copy or `uploadfs`
requirement. Reading this source never adopts a club or changes user values.
The combined DE/EN/short editor and complete collection import/export use the
common application draft. Reading the source never saves or groups competitions.

### Adopted names owner

Football provider fallback labels are volatile26/3-byte values. Both adopted
languages, short names, shared club identities, complete provider bindings,
aliases, league metadata and explicit ID exceptions belong to one durable
collection, limited to16,384B including header, indices and CRC. Missing fields
and present-empty strings remain different. Identity keys are full UTF-8,
nonempty and at most63B; labels never establish identity by themselves.

NVS AppConfig stores the exact names reference: slot, generation, length and
CRC. `names_a.bin` and `names_b.bin` are the two immutable version paths. A changed
collection is staged only in the inactive slot, checked completely, synchronized
and closed. One verified NVS commit publishes all settings and that exact names
reference. Settings-only saves do not open or rewrite names. An uncertain NVS
write preserves both versions and blocks further mutation until recovery/reboot.
Startup reads only the referenced version; missing/corrupt names report an error
without making unrelated settings unusable. There is no legacy lookup,
migration, repair journal, source archive or automatic preset adoption.

The device caches only adopted fields needed by the current visible projection.
A missing override uses fresh provider fallback on every view; an explicit empty
short remains empty. Reads and source preparation happen outside repository and
LVGL locks. The cache is bounded by visible output, not adopted-club count.

### Names formats and resource costs

The `components/names/` owner contains the catalogue, adopted collection codec,
file publication and effective visible labels. Its dependency on football is
limited to pure provider/value contracts; the football service does no names I/O.

The immutable source JSON remains `football-team-names` version1 with full text
values. Its importer checks the entire UTF-8 input before adopting a complete
prefix of at most26B for DE/EN and3B for shorts. Long source values therefore do
not fail merely because they need clipping. Keys, provider IDs, aliases and
league metadata are never clipped. No original-user/provider text archive is
created. Optional provider-original acquisition is separate from durable names.

Adopted collection JSON is version2. Its `texts` object holds every unique text
once, under a positive collection-local ID; club fields, aliases and exception
names use those numeric references. Empty strings have an explicit reference;
absent club fields stay absent. IDs are1..16,384; they are not provider identities.
Dictionary values must be unique and referenced. Adopted DE/EN/short references
already meet26/3. Export streams the checked pool and uses its offsets as local
IDs; import accepts arbitrary object order and resolves forward references.
Both formats preserve all provider-ID arrays, aliases and explicit exceptions.

The importer has one temporary32-KiB arena plus112B scalar/index metadata
(32,880B requested on the C6),
allocated only under shared heavy admission. Packed rows, sorted delta-coded
bindings, prefix-compressed keys and the deduplicated text pool share that
arena; there is no JSON tree, full-body copy, second wire image or resident
saved-club model. OOM is an explicit failure with the old reference retained.
The final existing FNS2 writer enforces the complete16-KiB file ceiling.
The full147-club source currently imports to10,900B; this fixture contains no
ID exceptions and does not establish an unlimited arbitrary-club capacity.

Collection export prepares one request-only grouping index before HTTP output.
It visits each binding and alias a bounded number of times, rather than scanning
all of them again for every club. Reverse-linked groups preserve numeric ID and
alias order. Text stays in the referenced file and streams once through the
shared dictionary; there is no second file image or saved-club model.

For `C` clubs, `B` total provider bindings and `A` aliases, requested index heap
is `2*(5*C+B+A)` bytes, or zero when there are no bindings/aliases. The existing
wire costs at least11B/club,2B/binding and4B/alias, so index heap is below the
16-KiB file ceiling without adding a row cap. The147-club preset needs2,272B;
the exact16,384B grouped regression needs9,600B. Its actual POSIX reads total
34,635 calls including preparation and output; the previous alias loop alone
required960,000 lookups. These are host I/O counts, not C6 timings.

`AdoptedExport` is a136B C6 stack lease, including the existing116B open-reader
owner. It holds the immutable file from prepare through checked write/close,
then frees the transient index on scope exit. Allocation/read failure before
output produces a normal HTTP error; later I/O/close failure aborts the stream
without a success terminator. Export and import do not overlap under heavy
admission; backup export also owns its existing AppConfig snapshot. No resident
RAM, filesystem quota, task or schema is added by this index.

The streamed Apply transport ceiling is1,057,792B. Its conservative64x wire
allowance accounts for fully escaped63B keys despite disk prefix compression;
a club costs at least12 wire bytes and fewer than640 JSON bytes, while other
sections expand by less than7x. Shared texts avoid quadratic repetition. This
ceiling allocates no body buffer and does not extend the request deadline;
large real WiFi transfers remain device acceptance work.

Names staging uses actual filesystem occupancy and the one shared replacement
workspace, with no unused future image quota or second reserve charge. Checked
I/O still handles allocation/metadata failures. Image quotas and partitions stay
unchanged. Football matches/tables/events/coverage are RAM-only; names do not
scan or evict another owner's files. Ordinary restart/power loss preserves
adopted names. Trial firmware refuses persistent changes. The confirmed factory
reset deletes adopted versions; the immutable firmware catalogue survives.

The template `include/secrets.h.example` is tracked. An exact current stored
record overrides bootstrap presets. Missing or partial record fields invalidate
the record; omitted bootstrap preset fields use their software defaults.
An invalid preset is skipped and its field name is logged without its value.
Settings are typed fixed storage in `components/settings/`; the NVS adapter is
`components/settings/settings_store.cpp`. A single AppConfig contains general
settings, the ordered competition list, display rules and the adopted-names
reference. `GET/PUT /api/v1/config` reads/applies that complete revision. General
settings, competitions and screens use nested pure codecs, not separate HTTP
writes. The browser retains one shared draft until Apply is acknowledged.

A competition has three distinct parts:

- source: provider, provider competition identifier and season;
- club: optional numeric team identity, selected label, own-game handling and
  explicit group membership;
- display: nullable enable/automatic/duration overrides for each situation and
  sporting screen type.

Numeric team0 means no selected club;1..4,294,967,295 are exact provider team
identities. Preset text is validated before conversion, including leading
zeros; JSON and records use the numeric value. Competition identifiers remain
provider text. The season belongs to that provider, not the device calendar.

Groups are derived only from explicit nonzero `club.group` memberships1..8.
No independent group list or group name is stored. The heading uses the first
member's selected club label. The group takes its highest member priority;
member order stays the configured competition order. Renaming a club never
changes membership. Changing a selected club leaves its visible membership for
explicit review in the editor.

Successful Apply publishes the entire configuration with its revision. A
separate provider-data generation changes for relevant provider/source inputs;
club labels, appearance and display rules do not erase same-source data.
Runtime routes project only identity/source/club facts and omit the attached
80B display preferences. The UI projection omits network credentials and
provider request plans. Appearance-only changes preserve the current manual
visit and navigation position. A new competition receives its device identity
and incarnation in the configuration owner after revision admission.

Network retains a291-byte projection containing only raw Wi-Fi credentials,
AP password, hostname, NTP server and antenna choice, replacing its704-byte
general settings snapshot. Main injects the configured immutable time-zone
reference and four-byte diagnostic preferences into their services. These
readers preserve raw SSID bytes and do not read unrelated credentials or screen
rules. The additional requested-zone pointer supports the same timed retry
after an allocation failure. Linked target deltas remain a separate measurement.

Former `SECRET_WIFI_SSID_1`/`SECRET_WIFI_PASSWORD_1` and `SECRET_TIMEZONE`
names deliberately stop the build. The time-zone preset is a supported
location label, such as `Europe/Berlin`, rather than a POSIX rule.

## Defaults and bounds

The Web UI reads `GET /api/v1/schema`; it does not maintain another defaults
table. Every rejected value is answered with the name of its field and the
reason (see [API.md](API.md)). Apply requires all desired sections; omitted secrets retain their values.
Unknown keys, incorrect types, malformed array lengths and invalid values
are rejected before one coherent configuration publication.

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
| Competition source | OpenLigaDB | one source per competition: provider; competition ID up to 32 bytes without control characters or a space at either end (requests percent-encode it); season empty (current season) or up to 16 decimal digits for OpenLigaDB, four digits for other providers |
| Selected club | none | numeric team0 (none) or1..4,294,967,295, selected label up to32 bytes, own-game handling, explicit group0 (none) or1..8 |
| Display names | empty | competition and selected-club labels stored when picked from a provider list, up to32 bytes, no control characters; provider fallback26/3 and adopted names remain separate |
| ESPN / demo / browser diagnostics | off | explicit switches |
| Overview screenshot (`overview_screenshot`) | off (`false`) | saved boolean; adds the screenshot trigger to Overview. Capture remains available under Debug independently of this preference |
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
| Brightness / night brightness | 100% / 10% | 0–100%; controls are offered only with a controllable backlight pin; 0 switches the backlight off |
| Rotation | 0 | 0–3 quarter turns |
| Language | German | German or English |
| Night mode / window | on / 23:00–07:00 | backlight-capable targets only; local civil time; equal endpoints mean empty; requires valid clock |
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
Night mode changes only the hardware backlight brightness. Background,
text, pills, rims and images keep their normal colours and opacity. The
current C6 has no backlight pin, so brightness and night controls are not
offered and stored brightness/night values have no effect. The common
settings model retains those fields for backlight-capable targets. A changed
value is not evidence of the physical brightness on the device.
Display design constants (compile time, `app_config.h`): font role sizes per
size class (`kTextRolePx`, caption/body/title/score; size classes split at
`kSizeClassSmallMax` 260 and `kSizeClassMediumMax` 400 px shorter side),
the semantic colours for warnings, errors, neutral states, the IP chip and
slideshow dots, `kGoalFlashS` (120 s), how long a Konferenz row stays
highlighted after a goal, `kStatusPillS` (5 s), how long the status pill is
shown after a status change, and `kStatusPillOpa` (204 of 255), the opacity
of its background.
Display resource constants also live there: `kDisplayDrawBufferFraction` 12,
`kDisplaySpiQueueDepth` 10, `kDisplayClearRows` 8 and the 256-byte best-effort
port-context probe. The LVGL port explicitly uses priority 4, 7168 stack bytes,
no core affinity, 500 ms maximum sleep, 5 ms tick and internal/default stack
capabilities, preserving the current port values. The renderer uses 72 draw
items (bytes derived from its item type) and 1280 text bytes. JPEG validation
uses 4096 aligned bytes under heavy-operation admission; the installed TJPGD
profile has 512-byte input blocks, RGB888 output and fast-decode mode 1.
These are resource choices and codec facts, not additional user settings.
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

`kImageSourceRetryMs` (10,000 ms)
retries the desired image after file/source/decode failure; a new file
revision is checked immediately. `kImageValidationTimeoutMs` (1,000 ms)
bounds each admitted full-file/fallback-resolution pass, reusing the existing
JPEG workspace. A longer candidate chain resumes in a later ordinary app
poll. Waiting for exclusive-operation admission is nonblocking and does not
hold that admission during the delay. `kImageTempFile` names the one replacement
temporary file. `kFileReplacementWorkspaceBytes` (40,960 B) is a shared replacement workspace:
admission requires actual available space of at least the larger of that value
and the candidate size. The candidate consumes the workspace; no second reserve
or unused future image quota is charged. Checked I/O still decides whether the
operation completes. None is a user setting.

Image ID exhaustion, the image-byte quota and insufficient replacement workspace
report `storage_full` (HTTP507). Actual POSIX `ENOSPC` at open/write/sync/close/
rename keeps the same capacity result. Actual `ENOMEM` reports `memory` (also
HTTP507); other syscall failures report an I/O error. The common portable files
boundary classifies errno once; its single project-local application code
`files::kStorageFull` is `0x7f000001`, outside the SDK component ranges and within
signed32 `esp_err_t`. It changes no quota or persistent format. Checked sync/close
preserves the first failure instead of allowing cleanup to overwrite its errno.
Names candidates and the JPEG validator use this same distinction: failure
never replaces the active referenced version. JPEG validation preserves its
first stdio error through checked close, with the existing absolute deadline
taking precedence when the operation has expired. Its1B error classification
replaces its previous1B failure flag; actual C6 Validation size stays24B and
the shared JPEG work buffer is unchanged. The common physical Candidate is20B
and its borrowed Context12B on C6; the names domain candidate is144B, up4B
temporary from140B. The integrated cutover adds4B static RAM and1,552B linked
Flash; it adds no heap allocation, task, filesystem history or quota. The primary
operation error remains available through error(); a failed abort reports its
own cleanup errno separately. Both causes remain observable.

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
watchdog profiles safe. These limits are not saved settings. The separate
`overview_screenshot` preference occupies one boolean byte in the aggregate
AppConfig's general payload; it creates no additional NVS key and changes neither image quota nor
LittleFS allocation. The active 240×240
C6 requests one temporary 9,600 B internal RGB565 strip plus allocator
metadata, released after success or cancellation. See [API.md](API.md) for
the wire protocol and [WEB_UI.md](WEB_UI.md) for access and download behavior.

The current application envelope is version **1**, explicitly little-endian.
It has one 8-byte version/length/CRC32 header, then revision u32, last-apply ID
u64, exact names reference (slot u8, generation u32, length u16, CRC32 u32), the
693-byte general payload, variable competition payload (6 +209 bytes per
entry) and216-byte global screen-policy payload. At eight competitions the whole
record is **2618 bytes**. The screen rules, general fields and list retain
separate typed sections but no separate NVS commit.

The decoder accepts only exact current lengths and validates the entire model,
provider access, stable competition identities, attached display preferences and
names reference. Truncated, unknown-version, corrupt or invalid records select setup
defaults; no legacy record or shorter trailing-field migration is loaded.
Reading never repairs or writes a record. An unreadable record refuses writes.

One settings writer serializes Apply and explicit image-reference cleanup.
Snapshots use the short model lock. Apply validates the caller's expected revision, writes
one complete blob, commits and verifies readback before publishing the complete
RAM config and matching generation. There are no separate section setters. Provider-data generation advances only when its inputs change.
Deleting a competition removes its attached display preferences in that same
commit. Changed names are staged and validated first; the same NVS commit
selects their exact slot/generation/length/CRC before coherent activation.
Settings-only Apply never opens or rewrites the names file.

A write/commit/readback error may have reached NVS. It keeps the old coherent
RAM snapshot, reports `outcome_unknown` and refuses further writes until reboot
or explicit recovery. Immediate readback of old data is not proof of rollback.
A subsequent boot can load the old or new complete record; it cannot mix three
separately committed sections. Physical power-interruption acceptance remains
an owner-flashed device test.

The established NVS startup recovery erases the entire NVS partition only
for `NO_FREE_PAGES` or `NEW_VERSION_FOUND`, with an error log. That also
clears radio calibration and the request counters. Other NVS errors
retain RAM defaults and refuse persistent saves.

## Reset and firmware updates

The existing confirmed factory reset clears settings/competitions, request
counters and adopted names. Image-only deletion remains
an explicit option: it removes the library and crest/background replacements.
Full storage repair is the same reset workflow with `format_storage`; it
intentionally deletes setup and every LittleFS file, including stock assets.
A standalone repair confirmation cannot authorize that reset and returns an
actionable factory-reset-required error instead of formatting.

Settings and counter reset must succeed before deletion/formatting. Full format
is the explicit deletion step when LittleFS cannot mount; a mount fault never
automatically formats it. Each failed stage reports completed side effects,
and only complete success schedules restart. There is no RAM/NVS restoration
of adopted names. The firmware name source survives formatting; stock images
still use their established USB restoration path, and custom images can be
uploaded again through the Web UI.

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
day; the next UTC day writes a new record. There is no older counter-format fallback or migration. The settings reset in the Web UI also erases every counter, so a
damaged counter can always be cleared without a serial tool. This is a local
limit, not a guarantee against other devices sharing the provider quota or
IP address.

Routine firmware OTA preserves LittleFS. The current92-byte image descriptor
is version2 and declares the aggregate application format1. Firmware and browser
check the same descriptor version, target, partition layout and minimum aggregate
format. A descriptor1/general-record image is obsolete and rejected. A newer
aggregate format can be installed but development formats start fresh; no
settings, competition or backup conversion is implied. Current-version export
and import remain the supported way to transfer configuration and adopted names.
Trial firmware refuses
settings, competition and image writes until local boot acceptance, and loading
writes nothing, so automatic
rollback can still read the untouched record and files. Rollback does not undo shared
storage after an accepted image has subsequently saved new settings.

There is no startup legacy-image migration or removal pass in current firmware.

## Football data lifetime and adopted names

The football service keeps provider data only in RAM. Boot and Demo exit
acquire it afresh through normal provider limits and readiness. No competition
cache, schedule-evidence file, persisted completion time or next-game checkpoint
exists. Request counters and the discovery run/request guard remain in NVS.

Adopted names use the independent bounded16-KiB collection described above;
AppConfig's exact reference selects one of two immutable versions. Complete
collection import replaces the names draft and common Apply publishes it with
configuration. No48-row short-name transfer limit, football quota, cache
write gap, eviction or persisted schedule path belongs to this flow.

## SDK configuration

Shared SDK options live in `sdkconfig.defaults`; target-specific options live
in the selected `targets/*.sdkconfig.defaults`. Generated `sdkconfig.*` files
are build outputs. The build fingerprints the inputs, regenerates a stale
configuration and checks every explicit option against the generated result.

Continuous mDNS browsing is disabled. The network service uses only hostname
registration, HTTP-service advertising and mDNS startup, with no browse calls.
The local mDNS component's Kconfig explicitly supports disabling browsing for
advertisers, and its CMake excludes `mdns_browser.c` and `mdns_cache.c` under
this option. Hostname/HTTP advertising and `.local` DNS resolution stay enabled.

LVGL GRID and FLEX are disabled. Product scenes own their geometry; the
developer colour test positions its three wrapped labels from their measured
heights and checks the whole content band against SafeArea. Its red/green rings,
white title/detail and blue version remain. Automatic themes are disabled:
scenes/rings clear generic styles, images have no theme-specific styles, root
backgrounds and label font/colour/alignment are explicit. Observer bindings
are disabled because no subject/observer APIs are used. RGB888 remains enabled
because JPEG supplies that format before conversion to the RGB565 display.

The log tag cache and linked list are disabled; product code never changes
individual tag levels. The global dynamic level and INFO threshold remain
enabled, including direct calls from independently compiled SDK libraries.
The only linked tag setter is WiFi startup, which sets four tags to the existing
INFO default. Periodic health reporting and browser diagnostics keep their own
controls. Disabling dynamic levels as well would bypass the runtime threshold
in this SDK, so it is deliberately retained. This removes unused tag lookup
machinery; it does not disable logs, warnings, errors or product diagnostics.

In the C6 link before these two disables on 2026-10-18, the browse/cache objects
contained 6,442 B of linked code/constant/initialized data and 13 B of BSS; GRID
contained 3,469 B and no BSS. These are attributed predecessor payloads, not a
measured net saving: conditional code, alignment and link optimization can
change the final delta. The final matched totals below include the combined
change; they do not attribute a separate exact saving to each switch. Neither
option adds a buffer, task, heap allocation or filesystem data. The subsequent
theme/FLEX/observer/tag-cache predecessor object sizes likewise are not net
savings. The final whole-image values below include their combined result. Wireless
security, TLS verification and the trusted-root bundle are unchanged.

## Resources and acceptance

Current C6 compiler layout (2026-10-18): Model704B, Competitions1708B, RuntimeRoutes1060B,
Policy260B, AppConfig2704B, UiConfig448B (including derived Program206B),
PreparedContent116B, ViewModel980B, shared JSON Reader48B/Writer64B.
WebSettings131B, NetworkSettings291B and ProviderSettings284B are the narrow
consumer copies. The previous full general/policy owners and independent
competition-rule matrix are retired. These types are included in linked
static RAM when resident; do not add their sizes again to the linker total.

The exact-current NVS application record is at most2618B (earlier aggregate
checkpoint2882B). Internal read/modify/Apply helpers may request one2704B
candidate plus2618B wire; common HTTP Apply releases the optional32880B names
import arena before the NVS wire allocation. During import the candidate and
arena request35584B plus allocator overhead; there is no body/DOM copy.
Names export uses its separate bounded grouping index described above.

The current C6 quality-remediation build on2026-10-18 measures linked
Flash1,751,182B, staticRAM178,740B, binary1,751,936B and Webgzip92,683B.
The nine source remedies and focused integration checks are complete;
keyed-provider and physical acceptance remain open. The pre-consolidation baseline was
1,766,298/161,012/1,767,056/80,187B: net-15,116/+17,728/-15,120/+12,496B.
Individual object sizes are not additive to the linked totals.

The2026-10-18 generator-publication follow-up rechecked the active C6 build:
Flash/staticRAM/binary/Webgzip deltas are0/0/0/0B. All three generated build
files, firmware ELF and binary retain their exact hashes; the binary SHA256 is
`32ffb03a8b1fe17db8b24c1df05d91669f9cce4939b273486fbe469f903de058`.
No firmware heap/stack/fragmentation, task, NVS or LittleFS allocation changes.

The unchanged1,835,008B OTA slot has83,072B free. The standard85%limit is
1,559,756B; the binary exceeds it by192,180B. On2026-10-18 the owner explicitly
suspended this occupancy criterion for this alpha and its current functionality/
budget ([ADR-045](DECISIONS.md#adr-045--alpha-only-ota-reserve-exception--accepted-2026-10-18)).
The measurement still exceeds85%; the exception is not runtime or OTA acceptance.
Partition, image quota, security and all other resource criteria are unchanged.
No agent upload is authorized.

One provider response workspace can request7680B. A fixture index adds3392B;
the separate fixture-admission pass can add7296B ordinary-event staging. Those
two extras do not overlap:11072B or14976B requested payload before
TLS/HTTP/allocator overhead. Metadata and empty responses need no workspace.
These are C6 compiler layouts, not observed heap peaks. The system operation
Guard serializes those paths with names, image validation and OTA.
Normal JPEG rendering in the fixed LVGL pool and ordinary status576B can still
overlap provider traffic. JPEG validation uses4096B; device screenshot uses
one9600B internal strip. The two DMA draw buffers total19200B at240×240.
None of these allocation bounds proves an observed peak or free reserve.

Current task stacks remain provider12800B, HTTP10240B, LVGL7168B,
events2304B, DNS2048B and debug4096B. Individual compiler frames exclude callees,
SDK paths and actual task high water. Physical free heap/largest block,
fragmentation, all-task25%usable-stack headroom, C6 font-decode CPU, NVS garbage
collection/power interruption, LCD/SPI completion, OTA/rollback and longrun
acceptance require owner-flashed hardware. Current live keyed-provider states
also remain unverified. A fitting build is not full acceptance.

The supplied2026-10-18 short C6 run contains nine complete14-task samples through
272.6s after boot. Reported heap minimum reaches77,072B (lifetime sum of region
minima, interval start0ms); sampled free heap119,704..156,940B and largest free
block86,016..92,160B. These are not a simultaneous worst-case heap trace.
Lowest logged free stack: provider5,136B, HTTP5,800B, LVGL2,956B, main3,700B,
events1,888B, DNS1,336B and debug3,320B. The serial rows omit usable stack sizes,
so the all-task25% usable-stack criterion remains unverified. LVGL pool peak
8,956B and sampled fragmentation0..1% describe that pool only. LittleFS grows
from20,480 to126,976/458,752B (331,776B free at the last sample); file contents
and the reason for that growth cannot be inferred from these totals.

Two `POST /api/v1/images/upload` calls return `409 busy` during provider traffic.
The upload handler rejects an unavailable shared operation Guard before
beginning the upload. The log does not identify the holder or show a later
successful upload. All34 logged provider replies are HTTP200, largest410,681B;
HTTP success alone does not establish parser, displayed-data or feature acceptance.
No panic/reset is visible in the supplied excerpt. Screens, maximum-load/longrun,
power-loss and OTA/rollback checks remain separate acceptance evidence.


The configuration service alone coordinates changed-name Apply. The names
service prepares a request-local44-B C6 lease containing exact expected/desired
references and activation/preserve operations. Under the existing writer lock,
configuration verifies the lease, validates the complete candidate, persists and
verifies NVS, activates the prepared names receipt, then publishes the new RAM
configuration and revision. No external caller owns the settings writer lock.
A settings-only Apply cannot change the names reference. A failure after durable
NVS but before receipt activation keeps old RAM, preserves both slots and uses
the same outcome-unknown/read-only recovery path. The lease adds no retained
allocation, file, task or second persistence owner.


Visible-name resolution requires the configuration revision that supplied the
view language. It reads names reference and revision together, rejects an
already stale request before changing targets/cache, and checks completion
again. The controller also discards scenes after a concurrent Apply. HTTP
status obtains language/revision from one narrow snapshot and returns a
configuration-conflict response when that lookup becomes stale. Name files
remain outside the football lock; unchanged visible labels still reuse cache.

### Settings device diagnostics

All `kSettingsTest` modes remain available. Sample and save-loop diagnostics use
an AppConfig snapshot and the real revision-checked Apply. Raw corrupt/version/
short/long record modes deliberately bypass validation for the next boot test.
The full-NVS mode fills its separate namespace, requires both the fill and the
actual Apply to return `ESP_ERR_NVS_NOT_ENOUGH_SPACE`, and compares complete
configuration fingerprints before reporting PASS. RAM, I/O and setup failures
are failures of this check. It cleans the fill namespace afterwards. Controlled
host SDK errors verify this decision; physical NVS exhaustion and power-cut
behaviour still require an owner-flashed device run.

Diagnostics retain one2704-B candidate and at most one2618-B wire buffer at a
time; the fingerprint wire is released before Apply allocates its wire. These
are requested payloads, not measured stack/heap peaks. Normal configuration
logging copies172B of printable fields and presence/length indicators rather
than the704-B complete model. Password and raw SSID bytes are never copied into
that log snapshot. Diagnostic code remains inactive when kSettingsTest is None.

Provider live-scope capacity (`kProviderLiveItems`), initial calendar request-cost
estimate (`kFixtureCalendarEstimatedDays`) and URL bound (`kProviderUrlBytes`)
live in app_config with the other resource limits. Protocol phase numbers and
provider endpoint text belong to their adapter/sequence implementation. The
former fixture week-horizon setting is removed: canonical fixture retention
selects complete rounds and previous/next-three own identities from the season.

Configured entry-ID limits are exported by the typed schema from the owning
C++ type; the browser keeps no separate numeric maximum. Imported entries ask
for new local identities at Save while keeping their provider/club data and
attached display preferences.
