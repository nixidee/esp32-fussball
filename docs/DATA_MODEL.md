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
`store_mappers.h` (provider JSON → store), `store_views.h` (what the screens
read), `football_files.h` (competition files and short names), `components/settings/include/settings_model.h`, limits in
`include/app_config.h`. The C6 build compiles and the store, mappers and
views are host-tested; device, fixture and maximum-load acceptance remain
open.

The [planned screen-system redesign](UI.md#planned-screen-system-redesign)
requires verified round identity distinct from phase date clusters, known
fixture coverage for long breaks, generalized details for selected own and
ordinary single matches, and period/added-time/estimate/freshness/shootout
information. The approved data foundation now has one shared detail mechanism,
protected own/ordinary capacities, a first confirmed live observation and a
distinct suspended state. The active controller and provider demand now share phase selection. Exact
ordinary details are staged until a complete response and final target recheck;
capacity, malformed responses and a selection change during an HTTP response
can leave an explicit loss notice. Compact schedule evidence has its own
worker owner and durable binding, independently of display-store rows. No ordinary game is relabelled as own.

## 1. Data sets

| Data set | Content | Where / lifetime | Size (C6) |
|---|---|---|---|
| Competition list | up to 8 entries: stable id and service-owned incarnation, active flag, one source, display names | own competition record (NVS), persistent | 1,260 B in RAM, record up to 1,262 B ([CONFIGURATION.md](CONFIGURATION.md)) |
| Screen policy | five phases, six ordered types, shared rules and field-wise competition overrides, appearance and frame settings | one committed snapshot in the existing settings service; dedicated NVS record | 964 B snapshot, 912 B record; service and controller snapshots |
| Match store | matches, tables, team directory, round labels, shared match details/events and notices | RAM, one static store, updated per provider response; reloaded from competition files | 32,824 B |
| Competition files | per active competition: its part of the match store, what its source resolved and when each data type last completed | LittleFS `comp_<id>.bin`, rewritten after new data (section 4.8) | about 3 KB for an 18-team league, one 4 KB block |
| Short names | the owner's replacements of team short names, per provider and team id | LittleFS `short_names.bin` and RAM, persistent until a settings reset | 48 entries of 32 B in RAM, file up to 1,548 B including the 12-B header |
| Selection page | one page of a provider's competition or club list for the setup wizard | RAM, per request | 64 entries of 131 B |
| Display view model | what one screen shows, copied from the store | RAM, one static owned model; a full copy would need its own storage | 1,880 B |
| REST overview | own match and per-competition state, computed from the store | per request | – |

## 2. Definitions

| Term | Meaning |
|---|---|
| Competition | one entry of the competition list: a league, cup or tournament at one provider; each competition has exactly one source. Its stable id (1–255) is used by the store, the competition files and the REST interface. |
| Tracked club / own club | the club id of the competition entry's source. Optional: an entry without a club shows the competition only. |
| Own match | a match of the tracked club. The Own match screen shows the one in its window, else the one with the kickoff nearest to now. |
| Screen rules | per-phase enable, automatic participation and visit time, inherited field by field or overridden per competition identity in the policy. |
| Round | the provider's matchday or cup round (number and/or label). |
| Matchday | the matches of a competition whose kickoffs follow each other with gaps of less than 48 hours; it decides how long matches stay stored. |
| Live window | normally from 30 minutes before kickoff to its bounded expected/reported end; a confirmed live match without kickoff uses the first live observation instead. No kickoff is invented. Live requests follow this window ([DATA_FLOW.md](DATA_FLOW.md)). |
| Konferenz block | a competition's applicable kickoff block; conferences are separate by competition (section 7.2). |
| Estimated bit | per value: derived by the device, not reported by the provider. |
| Principal score | the ordinary goals of the match (after extra time when played), never including shootout goals. |

## 3. Identity and values

- A match or team is identified by its numeric provider id together with
  its provider and competition. A non-numeric id is not stored.
- Unknown values use 255 (`kNoValue`, `kNoIndex`) or −32,768 for signed
  table numbers, never 0; a real zero stays valid. Missing values are shown
  as `--`, never as 0.
- Kickoff and final whistle are UTC seconds; the score-change time is
  monotonic seconds, so clock corrections do not reorder it.
- Short names hold 26 bytes, own-match full names 64, round labels 31,
  event player names 32; text is cut at complete UTF-8 characters; unknown
  glyphs use the font fallback.
- Numbers accept integral JSON numbers and decimal strings; booleans, null,
  fractions, overflow and out-of-range values are rejected. Goals 0–99,
  minutes 0–240, added minutes 0–120.
- Names and scores are copied into the store; nothing borrows JSON storage.

## 4. Records and fields

Provider columns: ✓ filled, (✓) partly, ✗ never, E = estimated by the
device. OL = OpenLigaDB, ES = ESPN, AF = API-Football, FD =
football-data.org. Mapping details: [DATA_FLOW.md](DATA_FLOW.md).

### 4.1 Competition entry (settings)

| Field | Meaning | Req. | Used for |
|---|---|---|---|
| `active` | entry participates in classification, demand and applicable views | – | per-phase policy supplies enabled/automatic/manual rules; no legacy mask |
| `primary` | the competition's only source: provider, competition id, season (empty = current), club id | competition id | requests; club id marks own matches |
| `competition_name` | display name | – | Own match caption, Table title, start screen |
| `team_name` | club display name | – | start screen title |

### 4.2 Store parts

| Part | Capacity (`app_config.h`) | Record size | Content |
|---|---|---|---|
| Matches | 224 (`kStoreMatches`), all competitions | 44 B | section 4.3 |
| Team directory | 240 (`kStoreTeams`) | 32 B | provider team id, provider, short name |
| Table rows | 224 (`kStoreTableRows`), every competition | 14 B | section 4.5 |
| Table stage | 64 (`kStoreTableStageRows`), one table being received | 14 B | rows of a new table until it replaces the stored one |
| Match details | 16 (`kStoreDetailMatches`): 8 protected own slots and 8 ordinary slots | 148 B | match identity, full team names, up to 12 event indices; ordinary slots require an explicitly selected identity |
| Events | 96 (`kStoreEvents`): 48 protected own events and 48 ordinary events | 84 B | section 4.4; one mechanism with separate capacity banks |
| Round labels | 24 (`kStoreRoundLabels`) | 32 B | provider round text, shared by all matches of the round |
| Notices | one per competition | 3 B | a match, table row, team or event did not fit (E19), plus the bits noted in the running refresh |

The whole store measures 32,824 B: +5,216 B for the additional details/events
and +896 B for 224 first-live timestamps. The new targeted canonical-detail
tests cover the capacity protections and shared mapping. Older fixed-size
assertions still require reconciliation with this explicitly approved change.

Team short name: OL `shortName`, ES `shortDisplayName` (partly English), AF
none (the full name is used), FD `shortName`. They come with the fixtures,
table and live responses; a non-empty name in a later response replaces
the stored one.

### 4.3 Match (44 B)

| Field | Meaning | OL | ES | AF | FD | Shown / used |
|---|---|---|---|---|---|---|
| `competition`, `provider`, `id` | identity | ✓ | ✓ | ✓ | ✓ | updates |
| `home`, `away` | team directory indices | ✓ | ✓ | ✓ | ✓ | Own match, Konferenz, REST |
| `round` | round number | ✓ | ✗ | ✗ | ✓ `matchday` | Own match caption "N. Spieltag" when no label; Konferenz matchday |
| `label` | round text (pool index) | ✓ `groupName` | ✗ | ✓ `league.round` | ✓ `stage` | Own match caption (preferred over the number) |
| `kickoff` | UTC kickoff | ✓ | ✓ | ✓ | ✓ | live windows, Konferenz blocks, countdown, matchday expiry, REST |
| `first_live_observed_utc` | first confirmed provider-running observation; never an invented kickoff | derived observation | derived observation | derived observation | derived observation | bounded selection when kickoff is unusable; persisted across polls and suspension |
| `finished_at` | final whistle: first seen finished, estimated | E | E | E | E | live window end, Konferenz block end |
| `phase` | Unknown … Abandoned (section 5) | (✓) finished flag, else E | ✓ | ✓ | ✓ | pill, live windows, Konferenz, rim, REST |
| `minute` | match minute | E (`~`) | ✓ | ✓ | ✗ | pill, rim, REST |
| `principal` | ordinary goals | ✓ | ✓ | ✓ | ✓ | score centre, Konferenz, REST, goal flash |
| `halftime`, `regulation`, `extra_time` | intermediate scores | ✓ (extra time at times incomplete) | ✗ | ✓ | ✓ | **not shown** |
| `shootout` | shootout tally | ✓ (derived) | ✓ | ✓ | ✓ | pill "i.E. 4 : 2" |
| `score_changed` | last principal score change | – | – | – | – | Konferenz goal flash (120 s), page order |
| `flags` | own match, events truncated | – | – | – | – | accent colour, own match choice |
| `estimated` | estimated bit per value (phase, minute, final whistle) | – | – | – | – | `~` minute |
| `own` | own-match slot (full names, events) | – | – | – | – | Own match |

### 4.4 Event (84 B)

| Field | Meaning | OL | ES | AF | FD | Shown |
|---|---|---|---|---|---|---|
| `type` | Goal, OwnGoal, PenaltyGoal, PenaltyMissed, Yellow, YellowRed, Red, Substitution, Injury, VAR | goals only | goals, cards, substitution, injury, VAR if supplied | all (no request plan asks for them) | ✗ | highlight line label |
| `minute`, `added` | minute and added minutes | (✓) | ✓ minute | ✓ | ✗ | highlight line (minute) |
| `actor`, `beneficiary` | side that acted / that profits (own goal) | derived | ✓ | ✓ | ✗ | **not shown** |
| `player` | player; for a substitution the player going off (an ESPN scoreboard substitution: both names "A / B") | ✓ | ✓ | ✓ | ✗ | highlight line |
| `player_in` | substitution: player coming on | ✗ | ✓ (match summary) | ✓ | ✗ | highlight "out > in" |
| `after` | score after the event | ✓ | ✗ | ✗ | ✗ | identity; **not shown** |
| `flags` | removed, shoot-out, origin (the detail request) | – | ✓ shoot-out | ✓ shoot-out | ✗ | removed events hidden; "Elfmeterschießen" |
| `identity`, `revision` | supplied id or hash; change fingerprint | – | – | – | – | updates |

### 4.5 Table row (14 B)

| Field | OL | ES | AF | FD | Shown |
|---|---|---|---|---|---|
| `team` | ✓ | ✓ | ✓ | ✓ | Table name |
| `position` | E (row order) | ✓ | ✓ | ✓ | Table "#" |
| `played` | ✓ | ✓ | ✓ | ✓ | Table "Sp" |
| `points` | ✓ | ✓ | ✓ | ✓ | Table "Pkt" |
| `difference` | ✓ | ✓ | ✓ | ✓ | Table "Diff" |
| `won`, `drawn`, `lost`, `goals_for`, `goals_against` | ✓ | ✓ | ✓ | ✓ | **not shown** |
| own flag | – | – | – | – | Table accent row, scroll position |

### 4.6 Store rules

- *Expiry:* a matchday (kickoffs less than `kMatchdayGapHours` apart) is
  *over* at the end of the local date of its last kickoff and *passed* at
  the end of the following local day; a passed matchday is dropped. The
  provider worker drops passed matchdays after every match fetch of a
  competition (so a passed matchday that a provider sends again never
  stays) and every minute (`kStoreMaintenanceS`). Matches without a kickoff
  stay. One module (`football/matchday`) holds the gap rule, "over",
  "passed" and the Matchday selection for the store, the views, the
  request planner and the display.
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
  competition notes it (E19). A broken or empty response keeps the stored
  table. OpenLigaDB positions come from the row
  order and are marked estimated.
- *Events:* matches with an allocated shared detail slot, in provider order per origin (the source's
  list request, or its detail request — the ESPN match summary with the
  substitutions), at most 12. Each origin replaces only its own events;
  the other origin's events stay ahead of a new list. A full list gives way
  first with detail events, then the oldest; dropping a shown event flags
  the match truncated. The event pool has protected 48-event own and ordinary
  banks: when its bank is full, the match farthest from now gives up
  its oldest event to a nearer one (live and next matches win); a match
  that loses events or gets none for lack of space is flagged and its
  competition notes it (E19) until no detailed match of it is flagged any
  more. An event missing from a later complete list of its
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
| `stale` | a request of an active competition failed and has not succeeded since, or a shown match is in its live window and its competition's last successful match request is older than 15 minutes (`kDataStaleS`) | status pill, REST |
| `retry_ms` | next scheduled request while a competition has failed | notice cards "Neuer Versuch HH:MM", REST |
| `competitions` | one state per competition (below) | status pill, notice cards, REST `competitions[]` |

Per competition (`CompetitionState`, index = position in the competition
list):

| Field | Meaning |
|---|---|
| `id`, `active` | stable competition id, competition active |
| `fetched_ms` | monotonic time of its last successful request (0 = none yet); a failed competition keeps its earlier data |
| `cause` | why its last request failed (`Cause`, keys and codes in [API.md](API.md#error-causes-and-codes)); `unmaintained` when a match of it kicked off more than 24 hours ago (`kUnmaintainedAfterS`), is neither finished, postponed, cancelled nor abandoned and has no result; otherwise `store_full` while it has a notice; none after a successful request |
| `provider`, `http` | provider of the failed request (or of the unmaintained matches) and its HTTP status (0 = none) |
| `stale` | its last request failed, or a shown match of it is in its live window and its last successful match request is older than `kDataStaleS` |
| `notices` | what of it did not fit the store: matches, table, match or table teams, events (`MatchStore::notices()`); REST `competitions[].missing` |
| `configured` | at least one active competition (or demo) | setup card, stock image |

### 4.8 Competition files and short names

A competition file lets a reboot show the data without a request. It holds
the competition's matches with their teams and round labels, its table, the
detailed matches' full names and events, its notices, what its source resolved
(season, OpenLigaDB round, ESPN match days, "no table") and the UTC time
each of fixtures and table last completed, and when API-Football's season
was last resolved. Not kept: the
monotonic time of the last score change (no goal highlight after a
reboot) and the live data types, which follow the live windows.

- **Format 4:** header, teams, round labels, matches (a detailed match followed by
  its full names and events), table rows, CRC-32. Structures are stored as
  in RAM, so a file is valid only for the build layout it names; another
  layout, version, a wrong count, index or CRC rejects the whole file.
  Format 3 is deliberately rejected and reloaded from the provider. The first
  confirmed live timestamp and detailed ordinary matches are kept. Short-name
  files retain their independent format and are unaffected.
- **Load:** at boot, for every active competition whose source is
  unchanged since the file was written. A rejected or foreign file is
  ignored and deleted later; a competition without a file waits for its
  requests as before.
- **Age:** loaded data count as old as the file. The requests of the kept
  data types are due when their interval since the saved completion has
  passed, measured once the clock is valid; live types run as usual.
- **Short names:** at most 48 replacements (`kShortNamesMax`) of 26 bytes,
  per provider and team id; an empty name removes one. They replace the
  provider's short name in every view (Konferenz, own match, table). The
  file has its own version and layout check, so a format change of the
  competition files keeps it. An invalid file starts with no replacements
  (a settings backup brings them back).

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

Principal goals, half-time, regulation, after-extra-time totals and the
shootout tally are separate values. Shootout goals are never added to the
principal score. An incomplete pair is shown as unknown.

OpenLigaDB has no live clock; its phase and minute are estimated from the
kickoff (rules in [DATA_FLOW.md](DATA_FLOW.md)) and the minute is shown with
`~`.

## 6. Events and corrections

Actual event coverage depends on provider and competition
([DATA_PROVIDERS.md](DATA_PROVIDERS.md)). Actor and beneficiary are separate;
an unknown reference stays empty. A supplied event id is preferred, else a
deterministic hash; a revision fingerprint tracks score and player changes.
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

Each row shows team short names, the principal score once started, else the
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
| Matches | 224 (`kStoreMatches`) | all competitions |
| Table rows | 224 (`kStoreTableRows`) | all competitions' tables |
| Teams | 240 (`kStoreTeams`) | all competitions |
| Own matches with full names and events | 8 (`kStoreOwnMatches`) | all competitions |
| Events | 48 (`kStoreEvents`), 12 per match (`kMatchEvents`) | own matches |
| Round labels | 24 (`kStoreRoundLabels`) | all competitions |
| Selection page | 64 entries | – |
| Stored matchdays | what the request plans fetch (current and next matchday, own fixtures within 7 days), until the end of the local day after the matchday | – |

Overflow handling is described in section 4.6.

## 9. Data needed but not in the model

| Data | Status |
|---|---|
| Competition kind (league / cup / tournament), list of rounds | not stored |
| Crest reference per team | not stored |
| Whistles (kick-off, half-time, 2nd half, full time, extra time) | only derived from the phase |
| Delays | not in the model |
| Group tables | one table per competition |

## 10. Known gaps at a glance

**Stored but never shown or used:** `halftime`, `regulation`, `extra_time`, event `added`, `actor`, `beneficiary`, `after`,
table `won`, `drawn`, `lost`, `goals_for`, `goals_against`, the
events-truncated flag (on screen).

**Provider-dependent display differences:**

- ESPN: no round, so no round in the Own match caption and matchday grouping
  by UTC date; substitutions without direction ("A / B"); no half-time or
  extra-time scores.
- OpenLigaDB: no minute (estimated), no cards or substitutions; extra-time
  result can be missing; table position from row order; 3. Liga and
  Regionalliga goal data incomplete.
- API-Football: round only as provider text, no short names, no events.
- football-data.org: caption shows `stage` text instead of "N. Spieltag"
  when `stage` is supplied (provider value format not verified), no events.

**Structural limits against the target use:**

- At most 8 competitions and 192 stored matches.
- Events only for own matches; Konferenz matches have scores only.
- After a restart only the active competitions with an unchanged source show
  their kept data; a competition file is written at most every 10 minutes
  and not during a live window, so the last minutes before a power loss
  can be missing.


## Schedule evidence

The worker owns the one evidence array independently of the224display matches.
At most two source/season spans per competition retain verified UTC bounds,
completeness/freshness and actual previous/next kickoffs. Every active source
counts, including sources without a selected club or enabled automatic screen.
The separate CRC file is816B and the authoritative NVS discovery guard48B;
replacement also requires temporary filesystem headroom. Source/order/access
fingerprints prevent stale cursor or credential-bound proof resurrection.

Before a normal/discovery match response, the persisted source proof is fenced
as partial. Raw accepted source rows can contradict it before store eviction;
failed or unidentified refreshes downgrade every complete span. Another
season's success cannot restore one that was contradicted. Only a new complete
proof does so; failed persistence leaves phase-break selection conservative.
The file's guard is a transaction snapshot; NVS alone owns run credits, which
survive filesystem formatting. See [DATA_FLOW.md](DATA_FLOW.md#schedule-evidence-and-provider-protocol--2026-10-17)
for acquisition, limits and provider-specific proof restrictions.
