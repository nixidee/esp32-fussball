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

## 1. Data sets

| Data set | Content | Where / lifetime | Size (C6) |
|---|---|---|---|
| Competition list | up to 8 entries: stable id, active flag, four screen switches, primary source, optional fallback source, display names of competition and club | own competition record (NVS), persistent | 2,459 B in RAM, record up to 2,467 B ([CONFIGURATION.md](CONFIGURATION.md)) |
| Fixture pairs | up to 8 links of a primary and a fallback match id, each naming its competition by id | competition record (NVS), persistent | part of the competition record |
| Match store | matches, tables, team directory, round labels, own-match events and notices of all competitions | RAM, one static store, updated per provider response; reloaded from the competition files at boot | 22,728 B |
| Competition files | per active competition: its part of the match store, what its sources resolved and when each data type last completed | LittleFS `comp_<id>.bin`, rewritten after new data (section 4.8) | about 3 KB for an 18-team league, one 4 KB block |
| Short names | the owner's replacements of team short names, per provider and team id | LittleFS `short_names.bin` and RAM, persistent until a settings reset | 48 entries of 32 B in RAM, file up to 1,552 B |
| Selection page | one page of a provider's competition or club list for the setup wizard | RAM, per request | 64 entries of 131 B |
| Display view model | what one screen shows, copied from the store | RAM | 1,420 B |
| REST overview | own match and per-competition state, computed from the store | per request | – |

## 2. Definitions

| Term | Meaning |
|---|---|
| Competition | one entry of the competition list: a league, cup or tournament at one provider (plus optional fallback). Its stable id (1–255) is used by the store, the fixture pairs and the REST interface. |
| Tracked club / own club | the club id of the competition entry's source. Optional: an entry without a club shows the competition only. |
| Own match | a match of the tracked club. The Own match screen shows the one in its window, else the one with the kickoff nearest to now. |
| Screen switches | per competition, which of Own match, Konferenz, Matchday and Table show it; Matchday and Table show their competitions one after another in list order. Every requested competition's table is stored. |
| Round | the provider's matchday or cup round (number and/or label). |
| Matchday | the matches of a competition whose kickoffs follow each other with gaps of less than 48 hours; it decides how long matches stay stored. |
| Live window | from 30 minutes before kickoff to the expected end of a match; live data are requested only inside it ([DATA_FLOW.md](DATA_FLOW.md)). |
| Konferenz block | the shown matches of all active competitions with the same kickoff time (section 7.2). |
| Estimated / fallback bit | per value: derived by the device, or supplied by the competition's fallback source instead of its primary provider. |
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
| `active` | entry is requested and shown | – | the screens switched on in `screens` |
| `screens` | one switch each for Own match, Konferenz, Matchday, Table (default all on) | – | which screens show the competition; the request plans fetch only the data of the switched-on screens (DATA_FLOW) |
| `primary` / `fallback` | provider, competition id, season (empty = current), club id | primary competition | requests; club id marks own matches |
| `fallback_active` | use the fallback source | – | combining (DATA_FLOW section 6) |
| `competition_name` | display name | – | Own match caption, Table title, start screen |
| `team_name` | club display name | – | start screen title |

### 4.2 Store parts

| Part | Capacity (`app_config.h`) | Record size | Content |
|---|---|---|---|
| Matches | 192 (`kStoreMatches`), all competitions | 40 B | section 4.3 |
| Team directory | 240 (`kStoreTeams`) | 32 B | provider team id, provider, short name |
| Table rows | 192 (`kStoreTableRows`), every competition | 14 B | section 4.5 |
| Own matches | 8 (`kStoreOwnMatches`) | 148 B | match id, competition, full team names, up to 12 event indices |
| Events | 32 (`kStoreEvents`) | 84 B | section 4.4 |
| Round labels | 24 (`kStoreRoundLabels`) | 32 B | provider round text, shared by all matches of the round |
| Notices | one per competition | 2 B | a match, table row, team or event did not fit; fallback route active; fallback failed |

The whole store measures 22,728 B (pinned by the test suite).

Team short name: OL `shortName`, ES `shortDisplayName` (partly English), AF
none (the full name is used), FD `shortName`. Core-data requests update the
short names of teams the store already holds; they add no teams.

### 4.3 Match (40 B)

| Field | Meaning | OL | ES | AF | FD | Shown / used |
|---|---|---|---|---|---|---|
| `competition`, `provider`, `id` | identity | ✓ | ✓ | ✓ | ✓ | fixture pairs, updates |
| `home`, `away` | team directory indices | ✓ | ✓ | ✓ | ✓ | Own match, Konferenz, REST |
| `round` | round number | ✓ | ✗ | ✗ | ✓ `matchday` | Own match caption "N. Spieltag" when no label; Konferenz matchday |
| `label` | round text (pool index) | ✓ `groupName` | ✗ | ✓ `league.round` | ✓ `stage` | Own match caption (preferred over the number) |
| `kickoff` | UTC kickoff | ✓ | ✓ | ✓ | ✓ | live windows, Konferenz blocks, countdown, matchday expiry, REST |
| `finished_at` | final whistle: first seen finished, estimated | E | E | E | E | live window end, Konferenz block end |
| `phase` | Unknown … Abandoned (section 5) | (✓) finished flag, else E | ✓ | ✓ | ✓ | pill, live windows, Konferenz, rim, REST |
| `minute` | match minute | E (`~`) | ✓ | ✓ | ✗ | pill, rim, REST |
| `principal` | ordinary goals | ✓ | ✓ | ✓ | ✓ | score centre, Konferenz, REST, goal flash |
| `halftime`, `regulation`, `extra_time` | intermediate scores | ✓ (extra time at times incomplete) | ✗ | ✓ | ✓ | **not shown** (fallback filling) |
| `shootout` | shootout tally | ✓ (derived) | ✓ | ✓ | ✓ | pill "i.E. 4 : 2" |
| `score_changed` | last principal score change | – | – | – | – | Konferenz goal flash (120 s), page order |
| `flags` | own match, events truncated | – | – | – | – | accent colour, own match choice |
| `estimated`, `fallback` | estimated and fallback bit per value | – | – | – | – | `~` minute; filling rules |
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
| `flags` | removed, shoot-out, origin (fallback source, or the primary's detail request) | – | ✓ shoot-out | ✓ shoot-out | ✗ | removed events hidden; "Elfmeterschießen" |
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

- *Expiry:* a matchday stays until the end of the local day after its last
  kickoff; the provider worker drops passed matchdays every minute
  (`kStoreMaintenanceS`). Matches without a kickoff stay.
- *Full store:* the match of another club farthest from now makes room
  when the incoming match is nearer or own; otherwise the incoming match is
  skipped and the competition notes it. The list stays sorted by kickoff.
- *Score change:* the time is set when both scores are known and differ
  from the stored ones. *Final whistle:* the time the match is first seen
  finished; for a match never seen running the earlier of that time and
  kickoff + 110 minutes; both are marked estimated. *Estimates:* OpenLigaDB phases and minutes follow
  the clock every minute.
- *Tables:* a new table is staged next to the stored one and replaces it
  only when the response was complete and every row fit; a broken or empty
  response keeps the stored table. OpenLigaDB positions come from the row
  order and are marked estimated.
- *Events:* own matches only, in provider order per origin (primary
  source, the primary's detail request — the ESPN match summary with the
  substitutions — or the fallback source), at most 12. Each origin replaces
  only its own events; the other origins' events stay ahead of a new list.
  A full list gives way first with fallback events hidden behind a primary
  event, then other fallback events, then detail events, then the oldest; dropping a shown event flags the match truncated. An event
  missing from a later complete list of its own origin stays once as a
  removal marker; a response that broke off keeps every previous event.
- *Fallback values:* a paired fallback match fills the primary match's
  missing or only estimated kickoff, phase, minute and five scores with its
  reported values (swapped pairs swap home and away); each filled value
  carries a fallback bit and stays until the primary reports the field
  itself. A fallback goal is hidden while the match holds a goal of the
  primary, a fallback missed penalty while it holds one of the primary's.
- *Settings changes:* a competition whose sources, fallback switch or
  fixture pairs change, that is removed or paused loses its stored data;
  the others keep theirs. A changed provider key or ESPN opt-in drops the
  data of the competitions that use that provider; a changed demo mode
  clears the whole store.

### 4.7 Published state

| Field | Meaning | Shown |
|---|---|---|
| `revision`, `generation` | publication counter and data settings generation | – |
| `fetched_ms` | oldest last successful request over the active competitions | status band "Stand HH:MM", REST `last_update_ms` |
| `stale` | a primary request failed and has not succeeded since, without the fallback route standing in, or a shown match is in its live window and its competition's last successful match request is older than 15 minutes (`kDataStaleS`) | status band, REST |
| `retry_ms` | next scheduled request while a competition has failed | "Keine Daten" card, REST |
| `error`, `cause`, `error_routes` | first failure text and cause, failed competitions | status band, cards, REST |
| `routing_notice` | fallback in use or fallback failed | REST only |
| `matches_truncated` | a competition notes that a match did not fit | REST / Web UI |
| `configured` | at least one active competition (or demo) | setup card, stock image |

### 4.8 Competition files and short names

A competition file lets a reboot show the data without a request. It holds
the competition's matches with their teams and round labels, its table, the
own matches' full names and events, its notices, what its sources resolved
(season, OpenLigaDB round, ESPN match days, "no table") and the UTC time
each of core data, fixtures and table last completed. Not kept: the
monotonic time of the last score change (no goal highlight after a
reboot) and the live data types, which follow the live windows.

- **Format:** header, teams, round labels, matches (an own match followed by
  its full names and events), table rows, CRC-32. Structures are stored as
  in RAM, so a file is valid only for the build layout it names; another
  layout, version, a wrong count, index or CRC rejects the whole file.
- **Load:** at boot, for every active competition whose sources are
  unchanged since the file was written. A rejected or foreign file is
  ignored and deleted later; a competition without a file waits for its
  requests as before.
- **Age:** loaded data count as old as the file. The requests of the kept
  data types are due when their interval since the saved completion has
  passed, measured once the clock is valid; live types run as usual.
- **Short names:** at most 48 replacements (`kShortNamesMax`) of 26 bytes,
  per provider and team id; an empty name removes one. They replace the
  provider's short name in every view (Konferenz, own match, table). An
  invalid file starts with no replacements.

## 5. Match phases and scores

Phases: Unknown, Scheduled, Live, HalfTime, ExtraTime, Penalties, Finished,
Postponed, Cancelled, Abandoned. Status and minute have independent
provenance. A missing status stays Unknown or explicitly estimated; elapsed
time never proves a final result. Postponed, cancelled and abandoned matches
are excluded from windows and Konferenz blocks.

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

A block is every shown match of every active competition with the same
kickoff time; postponed, cancelled and abandoned matches and matches without
a kickoff never take part. A block's matches are shown from kickoff minus the
before offset until the block ends (the latest expected end of its matches
plus the after offset) or the next block's before offset starts, whichever is
first. A match that overruns into the next block (it finished after that
start, or is still running) stays until its own end plus the after offset.
The expected end is the final whistle when known; a provider-reported
running phase allows up to 240 minutes after kickoff; otherwise 110 minutes
after kickoff are assumed. A match first seen as finished without having
been seen running gets an estimated final whistle of the earlier of now and
kickoff plus 110 minutes. With "show own match for the whole matchday" on,
the own match also stays from its kickoff while any other shown match
belongs to the same matchday (same round of the same competition; without
round data the same UTC date). Members are listed in kickoff order. The
relevant-matchday default screen applies while at least one match is a
member. Own cup or catch-up matches have their own windows and can select
Own match outside the league window.

Each row shows team short names, the principal score once started, else the
kickoff time or "--:--"; the own match in the accent colour; a goal flash
for 120 seconds after a score change. No events are shown. Without members:
the card "Keine Spiele im Zeitfenster" with the next kickoff.

### 7.3 Table

Every competition with the Table switch and a stored table, one after another
in settings order. Title: its display name. Columns position, name, played,
difference, points; columns are dropped from the right when the name would
get too short. With an own club the rows around it are shown and the next
competition follows after the table step (`table_page_s`); without one the
table pages through `table_window` rows per step, then the next competition
follows. Up/Down move row-wise; past the last (first) row the next (previous)
competition follows. All stored rows are reachable. While a match of the
competition runs, the live table interval refreshes it (DATA_FLOW). Without
any table: a card.

### 7.3a Matchday

Every competition with the Matchday switch, one after another in settings
order. Each shows its first stored matchday (kickoffs less than
`kMatchdayGapHours` apart): the current one until it expires (section 4.6),
then the next one. Rows as in the Konferenz, in kickoff order, with the
result or kickoff time; `visible_matches` rows per page, a new page every 8
seconds, after the last page the next competition. Caption: competition
display name · round label, else "N. Spieltag", else the date. Without any
matchday: the card "Spieltag / Keine Spiele".

### 7.4 Other users

- **Start screen:** club display name and the first active competition's
  provider name.
- **Status band / state cards:** stale state, fetch time, error, next retry.
- **REST status:** own match (competition, phase, kickoff, goals, minute,
  team names) and per competition the match count, provider and error flag
  ([API.md](API.md)). Tables and events are not exposed.

## 8. Capacities

| Item | Limit | Shared by |
|---|---|---|
| Competitions | 8 (`kCompetitionMax`) | – |
| Matches | 192 (`kStoreMatches`) | all competitions |
| Table rows | 192 (`kStoreTableRows`) | all competitions' tables |
| Teams | 240 (`kStoreTeams`) | all competitions |
| Own matches with full names and events | 8 (`kStoreOwnMatches`) | all competitions |
| Events | 32 (`kStoreEvents`), 12 per match | own matches |
| Round labels | 24 (`kStoreRoundLabels`) | all competitions |
| Fixture pairs | 8 | all competitions |
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

**Stored but never shown or used:** `halftime`, `regulation`, `extra_time`
(only fallback filling), event `added`, `actor`, `beneficiary`, `after`,
table `won`, `drawn`, `lost`, `goals_for`, `goals_against`, the
events-truncated flag (on screen), `routing_notice` (on screen).

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
- After a restart only the active competitions with unchanged sources show
  their kept data; a competition file is written at most every 10 minutes
  and not during a live window, so the last minutes before a power loss
  can be missing.
