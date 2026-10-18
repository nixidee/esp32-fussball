# Data model

What the device stores about football: every data set and record, the
meaning of each field, which provider fills it, where it is shown and under
which conditions, the capacities, and the known gaps.

Related documents:

- [DATA_FLOW.md](DATA_FLOW.md) — how the data is requested, mapped,
  combined and published.
- [DATA_PROVIDERS.md](DATA_PROVIDERS.md) — provider coverage, limits and
  endpoints.
- [UI.md](UI.md) — screen layout; [API.md](API.md) — the device REST API.

Implementation: `components/football/include/match_store.h` (store),
`provider_adapter.h` (typed provider requests/facts) and
`store_mappers.h` (typed facts → store), `store_views.h` (what the screens
read),
`components/names/include/` (`team_name_store.h`, `team_name_files.h` and
`effective_names.h`: durable names, transactions and prepared labels),
`components/settings/include/settings_model.h`, limits in
`include/app_config.h`. Store/mapping logic and the bounded names codec have
host checks. The coherent names owner is implemented; final linked resource
fit, device, real-provider and maximum-load acceptance remain open.

The [current screen system](UI.md#current-screens-and-automatic-sequence)
requires verified round identity distinct from phase date clusters, known
fixture coverage for long breaks, generalized details for selected own and
ordinary single matches, and period/added-time/estimate/freshness/shootout
information. The approved data foundation now has one shared detail mechanism,
protected own/ordinary capacities, a first confirmed live observation and a
distinct suspended state. The active controller and provider demand now share phase selection. Exact
ordinary details are staged until a complete response and final target recheck;
capacity, malformed responses and a selection change during an HTTP response
can leave an explicit loss notice. Compact schedule evidence has its own
worker owner and source binding in RAM, independently of display-store rows. No ordinary game is relabelled as own.

## 1. Data sets

| Data set | Content | Where / lifetime | Size (C6) |
|---|---|---|---|
| Application configuration | general Model, competition list, policy, exact NamesRef, revision and last-apply ID | one service AppConfig and one NVS `settings/app_config` blob | 2,704 B RAM; record at most 2,618 B |
| Competition list | up to 8 entries: stable id and service-owned incarnation, active flag, one source, display names | AppConfig competition section (one NVS envelope), persistent | 1,708 B section in RAM, encoded payload up to 1,678 B ([CONFIGURATION.md](CONFIGURATION.md)) |
| Screen policy | five phases, six ordered types, shared rules and field-wise competition overrides, appearance and frame settings | AppConfig policy section (one NVS envelope), persistent | 260 B global policy, 216 B payload; competition overrides are attached to competition entries |
| Match store | matches, tables, original team fallback directory, round labels, shared details/events and notices | RAM, one static store; reacquired from providers after boot | 46,928 B (C6 compiler layout), included in final174,200 B linked static RAM |
| Adopted club names | both languages, short code, stable keys, provider bindings, aliases, league metadata and explicit ID exceptions | one selected immutable LittleFS version, exact AppConfig reference, until explicit factory reset | complete file at most 16 KiB; no global saved-club/legacy RAM table |
| Club-name source catalogue | retained German/English/short names, aliases and verified provider-ID lists for 147 clubs | repository `include/team_names.json`, embedded read-only in the running app; streamed at `/api/v1/team-names/preset` | 47,451 source bytes; 12,767 B constant payload plus fingerprint/code/alignment; no retained RAM, heap or filesystem copy |
| Selection page | one page of a provider's competition or club list for the setup wizard | RAM, per request | 64 entries of 131 B |
| Display view model | what one screen shows, copied from the store | RAM, one static owned model; a full copy would need its own storage | 2,044 B |
| REST overview | own match and per-competition state, computed from the store | per request | – |

## 2. Definitions

| Term | Meaning |
|---|---|
| Competition | one entry of the competition list: a league, cup or tournament at one provider; each competition has exactly one source. Its stable id (1–255) is used by the store and the REST interface. |
| Tracked club / own club | the numeric team id in the competition entry's separate ClubPreference. An entry without a preferred club shows the competition only. |
| Own match | a match of the tracked club. The Own match screen shows the one in its window, else the one with the kickoff nearest to now. |
| Screen rules | per-phase enable, automatic participation and visit time, inherited field by field or overridden directly on a competition entry. |
| Round | the provider's matchday or cup round (number and/or label). |
| Matchday | the matches of a competition whose kickoffs follow each other with gaps of less than 48 hours; it decides how long matches stay stored. |
| Live window | normally from 30 minutes before kickoff to its bounded expected/reported end; a confirmed live match without kickoff uses the first live observation instead. No kickoff is invented. Live requests follow this window ([DATA_FLOW.md](DATA_FLOW.md)). |
| Konferenz block | a competition's applicable kickoff block; conferences are separate by competition (section 7.2). |
| Estimated bit | per value: derived by the device, not reported by the provider. |
| Principal score | the ordinary goals of the match (after extra time when played), never including shootout goals. |

## 3. Identity and values

- A match uses its competition, provider and positive numeric provider id.
  The shared team directory uses provider and positive numeric team id.
  Configured club id0 means no tracked club; positive values through
  4,294,967,295 identify a club. Invalid values are rejected at the boundary.
- Unknown values use 255 (`kNoValue`, `kNoIndex`) or −32,768 for signed
  table numbers, never 0; a real zero stays valid. Missing values are shown
  as `--`, never as 0.
- Kickoff and final whistle are UTC seconds; the score-change time is
  monotonic seconds, so clock corrections do not reorder it.
- Working labels hold one selected-language name of 26 UTF-8 bytes and one
  3-byte short code. Source-field fallback is selected before each independent
  byte limit is applied; a provided abbreviation that cannot fit one UTF-8
  character stays empty. Team labels are not repeated in match details.
  Round labels retain 31 bytes and event player names 32. Prefix clipping omits partial UTF-8 characters;
  unknown glyphs use the font fallback. IDs, keys, aliases and full league
  metadata are not clipped as display labels.
- Numbers accept integral JSON numbers and decimal strings; booleans, null,
  fractions, overflow and out-of-range values are rejected. Goals 0–99,
  minutes 0–240, added minutes 0–120.
- Names and scores are copied into the store; nothing borrows JSON storage.
  Fixed text and byte assignments also support their own existing storage
  or a subview of it without erasing the copied value.

## 4. Records and fields

Provider columns: ✓ filled, (✓) partly, ✗ never, E = estimated by the
device. OL = OpenLigaDB, ES = ESPN, AF = API-Football, FD =
football-data.org. Mapping details: [DATA_FLOW.md](DATA_FLOW.md).

### 4.1 Competition entry (settings)

| Field | Meaning | Req. | Used for |
|---|---|---|---|
| `active` | entry participates in classification, demand and applicable views | – | per-phase policy supplies enabled/automatic/manual rules; no legacy mask |
| `primary` | provider, competition id and season (empty = current); no selected club or display preferences | competition id | request/source identity |
| `club` | numeric preferred team, optional display name, own-special preference and explicit group membership | – | own-match classification; group ordering/focus derives from member priority |
| `display` | field-wise phase/rule overrides attached to this competition | – | effective current programme; dormant overrides survive disabled phases |
| `competition_name` | display name | – | Own match caption, Table title, start screen |

### 4.2 Store parts

| Part | Capacity (`app_config.h`) | Record size | Content |
|---|---|---|---|
| Matches | 512 (`kStoreMatches`), all competitions | 40 B | section 4.3 |
| Team directory | 256 (`kStoreTeams`) | 36 B | provider team ID, provider, original name26 and short3 fallback, both NUL-terminated |
| Table rows | 224 (`kStoreTableRows`), every competition | 12 B | section 4.5 |
| Table stage | 64 (`kStoreTableStageRows`), one table being received | 12 B | rows of a new table until it replaces the stored one |
| Match details | 16 (`kStoreDetailMatches`): 8 protected own slots and 8 ordinary slots | 20 B | match identity and up to 12 highlight indices; all team labels resolve through the directory; ordinary slots require an explicitly selected identity |
| Events | 96 (`kStoreEvents`): 48 protected own events and 48 ordinary events | 80 B | section 4.4; one mechanism with separate capacity banks |
| Shared labels | 128 (`kStoreRoundLabels`) | 32 B | round and table-group display text; staged and resident references are retained during collection |
| Source dataset status | 8 sources ×3 datasets | 24 B per dataset; 80 B per source | actual HTTP attempt/success times, cause/HTTP and complete/partial state; no parallel Slot flags |
| Round references | 64 (`kStoreRounds`) | 12 B | competition, reported round number/label, observed count, checked time, complete/partial state |
| Table groups | 32 resident and 32 staged (`kStoreTableGroups`) | 3 B | competition, provider-order ordinal and shared label index; no duplicated name arrays |
| Notices | one per competition | 3 B | a match, table row, team or event did not fit (E19), plus the bits noted in the running refresh |

The current C6 compiler layout is 46,928 B: 13,144 B above the previous
33,784-B store with 224 matches and 240 teams. Removing unused retained fields
first reduced that store by 2,816 B. Capacity now covers four 36-team and four
20-team competitions with three rounds (336 games), plus 32 previous/next own
games and bounded headroom. Teams and matches use distinct 16-bit references;
small detail/event/label pools keep byte indices. There is no per-competition
team ceiling. Final linked totals and physical peaks remain acceptance work.
Provided team identities must fit before a fixture's opponent and score are
published together. A full directory rejects that fixture update, preserves its
previous facts and reports partial data with capacity notice E19. A table whose
team cannot be admitted keeps the previous complete table.
Detail slots retain only match identity and event references. Their team labels
resolve through the admitted directory identities; there are no duplicate detail
name fields or separate directory-pressure fallback copies.

Provider full and short text is borrowed during ingestion. OL supplies
`shortName`, ES `shortDisplayName`, FD `shortName`; AF has no separate short.
The directory stores only the original compact fallbacks. A visible page
collects at most ten numeric provider/team identities under the repository
mutex. After that read returns, the names service resolves adopted text and
explicit short exceptions into the visible output. No all-team effective
name projection or provider full-name archive is retained. The same provider/ID
is shared across retained matches and tables; additional saved clubs do not
allocate more working rows.

### 4.3 Match (40 B)

| Field | Meaning | OL | ES | AF | FD | Shown / used |
|---|---|---|---|---|---|---|
| `competition`, `provider`, `id` | identity | ✓ | ✓ | ✓ | ✓ | updates |
| `home`, `away` | team directory indices | ✓ | ✓ | ✓ | ✓ | Own match, Konferenz, REST |
| `round` | round number | ✓ | reported calendar stage when supplied | ✗ | ✓ `matchday` | Single caption "N. Spieltag" when no label; Konferenz matchday |
| `label` | round text (pool index) | ✓ `groupName` | reported calendar stage when supplied | ✓ `league.round` | ✓ `stage` | Single caption (preferred over the number) |
| `kickoff` | UTC kickoff | ✓ | ✓ | ✓ | ✓ | live windows, Konferenz blocks, countdown, matchday expiry, REST |
| `first_live_observed_utc` | first confirmed provider-running observation; never an invented kickoff | derived observation | derived observation | derived observation | derived observation | bounded selection when kickoff is unusable; retained in RAM across polls and suspension |
| `finished_at` | final whistle: first seen finished, estimated | E | E | E | E | live window end, Konferenz block end |
| `phase` | Unknown … Abandoned (section 5) | (✓) finished flag, else E | ✓ | ✓ | ✓ | pill, live windows, Konferenz, rim, REST |
| `minute` | match minute | E (`~`) | ✓ | ✓ | ✗ | pill, rim, REST |
| `added`, `period` | reported stoppage minutes and explicit period; never inferred from elapsed time | ✗ | ✓ | explicit half status | ✗ | retained clock context |
| `principal` | ordinary goals | ✓ | ✓ | ✓ | ✓ | score centre, Konferenz, REST, goal flash |
| `shootout` | shootout tally | ✓ (derived) | ✓ | ✓ | ✓ | pill "i.E. 4 : 2" |
| `score_changed` | last principal score change | – | – | – | – | Konferenz goal flash (120 s), page order |
| `flags` | own match, events truncated | – | – | – | – | accent colour, own match choice |
| `estimated` | estimated bit per value (phase, minute, final whistle) | – | – | – | – | `~` minute |
| `detail` | optional shared event-detail slot | – | – | – | – | exact selected own or ordinary Single |

### 4.4 Event (80 B)

| Field | Meaning | OL | ES | AF | FD | Shown |
|---|---|---|---|---|---|---|
| `type` | Goal, OwnGoal, PenaltyGoal, PenaltyMissed, Yellow, YellowRed, Red, Substitution, Injury, VAR | goals only | goals, cards, substitution, injury, VAR if supplied | all (no request plan asks for them) | ✗ | highlight line label |
| `minute`, `added` | minute and added minutes | (✓) | ✓ | ✓ | ✗ | highlight line (minute) |
| `actor` | side that acted, including the conceding player for an own goal | derived | ✓ | ✓ | ✗ | event context |
| `player` | player; for a substitution the player going off (an ESPN scoreboard substitution: both names "A / B") | ✓ | ✓ | ✓ | ✗ | highlight line |
| `player_in` | substitution: player coming on | ✗ | ✓ (match summary) | ✓ | ✗ | highlight "out > in" |
| `flags` | removed, shoot-out, origin (the detail request) | – | ✓ shoot-out | ✓ shoot-out | ✗ | removed events hidden; "Elfmeterschießen" |
| `identity` | supplied id or deterministic hash | – | – | – | – | updates in place and removal detection |
| `ordinal` | reported order among shootout attempts | – | ✓ | – | – | stable attempt order, independent of the highlight list |

### 4.5 Table row (12 B)

| Field | OL | ES | AF | FD | Shown |
|---|---|---|---|---|---|
| `team` | ✓ | ✓ | ✓ | ✓ | Table name |
| `position` | E (row order) | ✓ | ✓ | ✓ | Table "#" |
| `played` | ✓ | ✓ | ✓ | ✓ | Table "Sp" |
| `points` | ✓ | ✓ | ✓ | ✓ | Table "Pkt" |
| `difference` | ✓ | ✓ | ✓ | ✓ | Table "Diff" |
| own flag | – | – | – | – | Table accent row, scroll position |
| `group` | ✓ | ✓ | ✓ | ✓ | exact provider table ordinal; groups never flatten into one ranking |

### 4.6 Store rules

- *Retention:* a successful complete fixture acquisition retains the previous,
  current and next reported rounds plus the previous own fixture and next three
  own fixtures. A finished result remains protected through the following local
  day. Incomplete acquisition never prunes the earlier scope. ESPN uses complete
  reported calendar stages; a single regular-season calendar uses explicitly
  derived date clusters under the shared `kMatchdayGapHours` rule. At a season
  boundary only the existing neighbours remain. Minute maintenance re-estimates
  provider-less phases and collects unused directory/label entries; it does not
  independently discard the retained rounds.
- *Full store:* the match of another club farthest from now makes room
  when the incoming match is nearer or own; otherwise the incoming match is
  skipped and the competition notes it (notice `store_full`, E19). The
  list stays sorted by kickoff.
- *Score change:* the time is set when both scores are known and differ
  from the stored ones. *Final whistle:* the time the match is first seen
  finished; for a match never seen running the earlier of that time and
  kickoff + 110 minutes; both are marked estimated. *Estimates:* OpenLigaDB phases and minutes follow
  the clock every minute.
- *Tables:* a new table is staged in its own buffer of
  `kStoreTableStageRows` (64) rows next to the stored tables and replaces
  the stored table of its competition only when the response was complete,
  every row fit the stage and the row pool holds it after the old rows of
  that competition are released; otherwise the stored table stays and the
  competition notes it (E19). Missing or broken responses keep the stored
  table; a successful explicit empty list clears it. OpenLigaDB positions come from the row
  order and are marked estimated.
- *Events:* matches with an allocated shared detail slot, in provider order per origin (the source's
  list request, or its detail request — the ESPN match summary with the
  substitutions), at most12 highlight references. Reported shootout attempts
  use separate references within the same96-event pool. Each origin replaces only its own events;
  the other origin's events stay ahead of a new list. A full list gives way
  first with detail events, then the oldest; dropping a shown event flags
  the match truncated. The event pool has protected 48-event own and ordinary
  banks: when its bank is full, the match farthest from now gives up
  its oldest event to a nearer one (live and next matches win); a match
  that loses events or gets none for lack of space is flagged and its
  competition notes it (E19) until no detailed match of it is flagged any
  more. A period/flag correction moves the same identity between highlights
  and shootout attempts without a duplicate or invented removal. An event missing from a later complete list of its
  own origin stays once as a removal marker; a response that broke off
  keeps every previous event.
- *Settings changes:* a competition whose source changes, that is removed
  or paused loses its stored data;
  the others keep theirs. A changed provider key or ESPN opt-in drops the
  data of the competitions that use that provider; a changed demo mode
  clears the whole store.

### 4.7 Published state

| Field | Meaning | Shown |
|---|---|---|
| `revision`, `generation` | publication counter and data settings generation | – |
| `fetched_ms` | oldest last successful request over the active competitions | status pill "Stand HH:MM", REST `last_update_ms` |
| `stale` | an active competition has a partial dataset or an unresolved dataset request failure, or a shown match is in its live window and its competition's last successful match request is missing or older than 15 minutes (`kDataStaleS`) | status pill, REST |
| `retry_ms` | next scheduled request while a competition has failed | notice cards "Neuer Versuch HH:MM", REST |
| `configured` | at least one active competition (or demo) | setup card, stock image |
| `competitions` | one state per competition (below) | status pill, notice cards, REST `competitions[]` |

Per competition (`CompetitionState`, index = position in the competition
list):

| Field | Meaning |
|---|---|
| `id`, `active` | stable competition id, competition active |
| `fetched_ms` | monotonic time of its last successful request (0 = none yet); a failed competition keeps its earlier data |
| `cause` | latest actual failed dataset request (`Cause`, keys and codes in [API.md](API.md#error-causes-and-codes)); otherwise `unmaintained` for a past unfinished match without a score, then `store_full` for a capacity notice. Another dataset's success never clears the failure. |
| `provider`, `http` | provider of the failed request (or of the unmaintained matches) and its HTTP status (0 = none) |
| `stale` | one of its datasets is partial or retains a request failure, or a shown match is in its live window and its last successful match request is missing or older than `kDataStaleS` |
| `notices` | what of it did not fit the store: matches, table, match or table teams, events (`MatchStore::notices()`); REST `competitions[].missing` |

Fixtures, table and events each own a24-B `DatasetStatus`: completeness,
64-bit monotonic last actual HTTP attempt/success, cause and HTTP status.
Unknown, Partial, Complete and Unavailable are distinct. The adapter declares
logical support: OpenLigaDB goal events arrive with fixtures, ESPN events with
summaries; API-Football and football-data.org events are Unavailable because
this integration maps none. Unavailable invents neither an HTTP failure nor
request/success timestamps. A same-source settings reload preserves supported
facts and errors; a changed source starts with its own capabilities and Unknown
supported datasets. Demo retains its actual fixture/table/event content.
Refusal before the first HTTP write does not invent a provider attempt.
Table/Matchday cards and timestamps select the matching dataset. Capacity and
unmaintained notices use the same dataset-specific rule, and fixture freshness
also applies the existing live-window age limit. HTTP copies the optional
576-B status projection under the football lock and sends it after unlocking.

### 4.8 Durable names and volatile football data

Matches, tables, events, round labels, resolved source season/round state and
schedule evidence have no persistent file. A reboot starts with an empty RAM
football store and normal provider acquisition; live windows and request
budgets still apply. There is no next-game checkpoint.

Adopted names use `FNS2` version1 under the complete-file16-KiB ceiling.
AppConfig's exact slot/generation/length/CRC reference selects `names_a.bin` or
`names_b.bin`. A changed collection is staged, checked, synchronized and closed
in the inactive slot; one NVS application commit publishes configuration and
its names reference together. An uncertain commit retains both versions until
recovery/reboot. Missing/corrupt referenced names remain an explicit error;
startup never substitutes an unreferenced file or silently adopts the preset.

Only current visible adopted fields are cached. Missing values use fresh RAM
provider fallback; present-empty shorts remain empty. There is no all-team names
projection, source archive, migration or standalone short-name write path.
Settings-only saves do not need the filesystem. Images retain their existing
quotas. Names never scan or evict image/football files; football files no longer
exist. Request counters and discovery run/request guards remain in NVS.

### 4.9 Club-name source catalogue

[include/team_names.json](../include/team_names.json) is an existing curated
source file, not a device store or a new names resolver. Its format is
`football-team-names`, version 1, with 147 entries in `clubs`, occupying 47,451
source bytes. Each entry has a stable `key`, the original `league` metadata,
preferred `de`, `en` and `short` strings, `aliases`, and
`ids: {"espn": [...], "openligadb": [...]}`. Every verified ID for the same club
can appear in its provider's array; an unknown identity is `[]`. The source
contains no credentials. League labels are retained input metadata and do not
establish current season membership.

Preferred German names, abbreviations and the first English CSV alternative
remain unchanged; additional explicit English alternatives and confirmed
provider spellings are retained in aliases. In the checked primary ESPN
responses, 71 preferred English texts occur literally in accepted team-name
fields. The other 76 preferred English texts are **unverified as ESPN English
name forms**: 40 have a confirmed ESPN club identity with a different provider
spelling, and 36 have no accepted ESPN identity in the checked evidence. A
provider ID establishes the club association, not the correctness of an English
translation. A literal provider spelling also does not establish the uniquely
correct translation. No factual translation or club-name error in the preferred
CSV German/English names was proved; formal prefixes, founding years and
transliterations were not silently corrected. Aliases and abbreviations must
not be assumed globally unique.

The complete source is embedded by `scripts/embed_team_names.py` as read-only
strings, fixed20-B club descriptors, alias references and provider IDs, without
pointers or constructors. The generated asset is build output, derived from
the single maintained JSON. The running app streams it losslessly at
`GET /api/v1/team-names/preset`; the response fingerprint identifies the
original source bytes. The combined German/English/short editor and full collection import/export
use the application draft. Common Apply is their adoption boundary; reading
the source never adopts all147 catalogue records automatically. Explicit full
provider-original acquisition remains separate from this durable codec.

The adopted-file codec uses a 96-byte little-endian header containing
generation, source fingerprint, section offsets/counts and CRC32; 9-byte club
records; 4-byte alias references; 7-byte provider/ID exceptions; and a
length-prefixed deduplicated UTF-8 pool. Full sorted keys use lossless prefix
compression, with 16-record restart checkpoints and an explicit 63-byte key
bound. Provider ID deltas and file-local club references are checked unsigned
variable-length values; each provider has 16-record disk checkpoints. File-local
club references belong to that atomic generation, never to catalogue order.
Absent fields and present-empty values remain distinct. Adopted DE/EN names
and short codes use the central 26/3 limits; league and alias texts round-trip
in full. No overflow sidecar or complete firmware JSON tree is used.

The complete current 147-club/288-binding/113-alias corpus with 48 exception
identities measures 11,241 B. Distinct maximal 26/26/3 club texts and 48
independent 3-byte exception texts, retaining the same other metadata, measure
15,572 B, leaving 812 B under the 16,384-B cap. The earlier 15,337-B case used
one shared deduplicated exception text; the distinct-text fixture removes that
optimism. These are measured fixtures, not a generic unlimited-club guarantee.
Additional custom identities/aliases consume the same byte quota.

One checked generation receipt is retained instead of a saved-club directory.
Whole-file validation happens on load/staging; addressed lookup requires the
same checked header and then decodes only its selected checkpoint block.
The current visible cache holds explicit name/short presence separately and
never retains an original-provider archive or a second all-team projection.

A missing selected-language value uses available provider full text, then
provider short, then numeric ID; the other maintained language is not selected
automatically. Provider fallback itself is volatile26/3. A missing short uses
provider short or available full fallback. Complete input is validated before
clipping. Stable IDs/keys/aliases establish identity; coincident labels never
merge clubs or change competition grouping.

Collection JSON version1 is the full-text source/import schema. Version2 is the
adopted export/Apply schema: unique `texts` dictionary entries and numeric text
references preserve shared long league/alias values without repeated copies.
Fields remain optional; explicit empty strings remain present. Every dictionary
entry is referenced; collection-local positive text IDs are unrelated to provider
IDs. The importer normalizes arbitrary field/club/ID ordering in one temporary
32-KiB arena and writes only an inactive candidate. It does not publish NVS.
One completed application Apply selects the exact version and settings revision.
Failed or uncertain acknowledgements use read-only state recovery, never an
automatic repeat of the mutation. No legacy record conversion is performed.

Adopted names survive ordinary reboot, power loss and firmware updates. The
existing confirmed factory reset deletes them. Full storage repair is a reset
option that intentionally deletes setup and files after settings/counters are
reset; it never promises restoration from a RAM/NVS name backup. The immutable
firmware source survives formatting, and image-only deletion remains separate.
Final linked resource fit and physical device acceptance remain open.

Exact length audit of the supplied147 preferred fields (including spaces):

| Unicode characters | German | English |
|---|---:|---:|
|28|1|0|
|27|1|1|
|26|1|0|
|Over28|0|0|

German28 is `SG Barockstadt Fulda-Lehnerz`; German27 is
`Borussia Mönchengladbach II` (28 UTF-8 bytes); English27 is
`Borussia Monchengladbach II`; German26 is `SpVgg Oberfranken Bayreuth`.
The source covers Bundesliga/second/third and five regional groups. It is not
an exhaustive independent current-roster/CL/legal-name translation check.
The28-byte observation does not override the newly selected26/3 runtime
bounds. Two clubs have three overlong DE/EN fields and23 source short codes
exceed3; the source records stay unchanged and their adopted values are clipped.

## 5. Match phases and scores

Phases: Unknown, Scheduled, Live, HalfTime, ExtraTime, Penalties, Finished,
Postponed, Cancelled, Abandoned, Suspended. Status and minute have independent
provenance. A missing status stays Unknown or explicitly estimated; elapsed
time never proves a final result. Postponed, cancelled and abandoned matches
are excluded from windows and Konferenz blocks.
Suspended is distinct from postponed: it retains an already justified active
slot within its running bound and freezes an existing minute. It does not
invent a new kickoff, continuing minute or final whistle. The new selection
controller applies that bound through the common selector.

The principal score and shootout tally are stored separately. Regulation
and extra-time parts are transient inputs to provider-specific score
derivation; half-time results are not retained. Shootout goals are never
added to the principal score. An incomplete pair is shown as unknown.

OpenLigaDB has no live clock; its phase and minute are estimated from the
kickoff (rules in [DATA_FLOW.md](DATA_FLOW.md)) and the minute is shown with
`~`.

## 6. Events and corrections

The twelve highlight references exclude shootout attempts. `view::shootout()`
reads every reported retained attempt from the same96-event pool in provider
order, including misses when the source actually reports them. A scored-to-missed
correction keeps the attempt identity. Missing attempts are never fabricated from
a final tally. Pool overflow sets the existing loss flags/notice.

Actual event coverage depends on provider and competition
([DATA_PROVIDERS.md](DATA_PROVIDERS.md)). The actor is retained; an unknown
reference stays empty. Consecutive OpenLigaDB scores derive the actor locally,
including the opposite side for an own goal. These intermediate scores and
beneficiary are not stored. A supplied event id is preferred, else a
deterministic hash. Repeated identities update the event in place.
An event missing from a later complete list stays once as a removal marker
and is hidden. Events stored before a restart are lost with the store; the
first response after a restart does not replay goals as new (no score
change time without a previous score). The 32-bit hashes are bounded;
collision and correction fixtures still need tests.

## 7. Usage per screen

### 7.1 Own match

- Shows the own match in its window (kickoff minus the before offset until
  the end plus the after offset), else the own match with the kickoff nearest
  to now; without any own match the card "Eigenes Spiel / Kein Spiel".
- Caption: competition display name · round label, else "N. Spieltag", else
  nothing (ESPN has neither, so the caption is only the competition name).
- Centre: principal score, before kickoff the local kickoff time.
- Pill: minute (`~` when estimated), "Halbzeit", "Elfmeterschießen",
  "Beendet", or the shootout tally; before kickoff a date pill and a
  countdown line.
- Highlight line: the stored events of this match, rotating every five
  seconds, without removed ones. With OpenLigaDB only goals appear; with
  football-data.org none.
- Rim: progress from the minute and phase.

### 7.2 Konferenz

The canonical selector resolves membership within one active competition,
using own and other pre/post windows and bounded running/interruption evidence.
Conferences never mix competitions. Whole-matchday own retention and legacy
screen masks have been removed. Unknown kickoff does not become an invented
time; first-live observation can provide its bounded basis. Own highlighting
remains when special-own treatment or pinning is off.

All-pages preserves chronological membership; One-page ranks activity, with
complete manual paging. Pinning reserves one row only when the own fixture is
present. Shared Conference/Matchday maximum starts at2; one actual fixture
shows alone. Remaining unique fixtures keep their original ceil(count/capacity)
page count. For more than two pages each earlier donor gives at most one row;
for two pages up to two rows. Later donors give first, preserving larger pages
at the front and no larger last page. Example unpinned counts5,5,5,2 become
5,4,4,4; a repeated pin is added afterwards.

Each row shows the prepared full team labels (26 UTF-8 bytes), the principal
score once started, else the
kickoff time or "--:--"; the own match in the accent colour; a goal flash
for 120 seconds after a score change. No events are shown. Without members:
the card "Keine Spiele im Zeitfenster" with the next kickoff.

### 7.3 Table

Enabled Table entries follow competition order in the common sequence.
Columns remain position, name, played, difference and points, with bounded
name-aware fitting. Automatic pages centre around the club or page through
all rows without one. Manual held row navigation reaches all rows. Visit time
is divided across pages with five seconds minimum per page; there is no
independent table clock. Empty loaded tables are skipped; missing/failed data
retain actionable status. Live-table demand follows current effective rules.

### 7.3a Matchday

Enabled Matchday entries follow competition order. The selected verified
sporting round retains known rescheduled fixtures outside the phase date
cluster. Current/next mode and held manual access choose the available round:

- *Current matchday* (default): the first stored matchday that has not
  passed, so a finished matchday stays until the end of the following
  local day, then the next one.
- *Next matchday*: the first stored matchday that is not over, so it moves
  on at midnight after the last kickoff. A competition without such a
  matchday is skipped by the automatic cycle.

Reachable with the buttons are, per competition, the stored matchdays from
the first one not passed up to the one after the selected one (at most
three: held, selected, next). Rows as in the Konferenz, in kickoff order,
with the result or kickoff time; `visible_matches` rows per page, a new
page according to the shared visit duration (at least5s each). The common
sequence selects the next entry after the visit. Caption:
competition display name · round label, else "N. Spieltag", else the date.
Without any matchday: the card "Spieltag / Keine Spiele".

### 7.4 Other users

- **Start screen:** club display name and the first active competition's
  provider name.
- **Status pill / state cards:** stale state, fetch time, cause and error
  code per competition, next retry.
- **REST status:** own match (competition, phase, kickoff, goals, minute,
  team names) and per competition the match count, provider, cause, error
  code and last update ([API.md](API.md)). Tables and events are not exposed.

## 8. Capacities

| Item | Limit | Shared by |
|---|---|---|
| Competitions | 8 (`kCompetitionMax`) | – |
| Matches | 512 (`kStoreMatches`) | all competitions |
| Table rows | 224 (`kStoreTableRows`) | all competitions' tables |
| Teams | 256 (`kStoreTeams`) | all competitions |
| Protected own-match event details | 8 (`kStoreOwnMatches`) | all competitions |
| Shared match details | 16 (`kStoreDetailMatches`): 8 protected own slots and 8 ordinary slots | all competitions; ordinary details require an explicitly selected match identity |
| Events | 96 (`kStoreEvents`), 12 highlights per match (`kMatchEvents`); shootout attempts use separate references in the same pool | 48 protected own events and 48 ordinary events in the shared mechanism |
| Shared round/group labels | 128 (`kStoreRoundLabels`) | all competitions, including an in-flight table transaction |
| Selection page | 64 entries | – |
| Stored rounds | previous/current/next complete rounds plus previous own/next3 own fixtures; selected ESPN calendar scope at most16 days | all competitions; no per-competition team ceiling |

Overflow handling is described in section 4.6.

## 9. Data needed but not in the model

| Data | Status |
|---|---|
| Competition kind (league / cup / tournament) | no separate classification field; provider-reported round/stage references are retained |
| Crest reference per team | not stored |
| Whistles (kick-off, half-time, 2nd half, full time, extra time) | only derived from the phase |
| Delays | not in the model |
| Group tables | separate provider-order groups, with independent rows and captions; no flattened ranking |

## 10. Known gaps at a glance

Reported period/stoppage, event actor/added minutes, estimates and data freshness
feed the Single projections. Shootout pages rotate through actual retained
attempts and expose incomplete reporting. Unused score parts, table W/D/L and
goals-for/against, and event revision/beneficiary/post-event totals are transient
mapper inputs or omitted. Physical display acceptance remains open.

**Provider-dependent display differences:**

- ESPN: reported calendar stages provide round labels where present; a single
  regular-season calendar uses derived date clusters. Scoreboard substitutions
  can lack direction ("A / B"); summaries can report both players separately.
- OpenLigaDB: no minute (estimated), no cards or substitutions; extra-time
  result can be missing; table position from row order; 3. Liga and
  Regionalliga goal data incomplete.
- API-Football: round only as provider text, no short names, no events.
- football-data.org: caption shows `stage` text instead of "N. Spieltag"
  when `stage` is supplied (provider value format not verified), no events.

**Structural limits against the target use:**

- At most 8 competitions and512 stored matches.
- Event details are bounded to8 protected own slots and8 ordinary slots;
  ordinary details are committed only for the explicitly selected
  identity after complete-response validation and a final selection recheck.
  The shared mechanism does not retain full details for every conference row.
- A restart begins with an empty football store. All fixtures, tables, events,
  source selections and schedule evidence are reacquired; there is no football
  checkpoint, competition file, filesystem cache or restore path.


## Schedule evidence

The worker owns the one evidence array independently of the512display matches.
At most two source/season spans per competition retain verified UTC bounds,
completeness/freshness and actual previous/next kickoffs. Every active source
counts, including sources without a selected club or enabled automatic screen.
This evidence is volatile. A current-only 36-byte NVS guard persists two
operational counters and whether their run is still active. It stores no
provider route, season, traversal step or source fingerprint. A reboot restarts
the RAM traversal with the remaining durable credits; unused reserved credits
are forfeited. The active run keeps its original credit lifetime across UTC
midnight. Changed source inputs retire the old run before any replacement
request; that UTC day cannot start another run. During a normal/discovery match
response, the RAM streaming flag suspends proof use until completion. Raw accepted source rows
can contradict proof before store admission; failed or unidentified refreshes
downgrade complete spans. Another season's success cannot restore a contradicted
span. Only a new complete proof does so. A reboot starts with Unknown evidence,
while NVS run credits survive filesystem formatting. ESPN calendars support
ordinary fixture retention but do not prove a match-free interval; the extra
discovery lane therefore sends no ESPN requests and keeps that source Unknown.
See [DATA_FLOW.md](DATA_FLOW.md#schedule-evidence-and-provider-protocol)
for acquisition, limits and provider-specific proof restrictions.


**Device acceptance remains open:** checked allocation/I/O failures preserve
published references, but host/POSIX replay is not evidence for physical
LittleFS metadata behavior, power cuts, NVS garbage collection, heap/stack
reserves or slow WiFi timing. The former impossible compile-probe fence and
future-unused-image-quota charge are retired.
