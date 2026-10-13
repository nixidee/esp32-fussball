# Data model

What the device stores about football: every data set and record, the
meaning of each field, whether it is required, which provider fills it,
where it is shown and under which conditions, the capacities, and the known
gaps.

Related documents:

- [DATA_FLOW.md](DATA_FLOW.md) — how the data is requested, mapped,
  combined, cached and published.
- [DATA_PROVIDERS.md](DATA_PROVIDERS.md) — provider coverage, limits and
  endpoints.
- [UI.md](UI.md) — screen layout; [API.md](API.md) — the device REST API.

Implementation: `components/football/include/football_model.h`,
`components/settings/include/settings_model.h`, limits in
`include/app_config.h`. The C6 source compiles; fixture, concurrency and
maximum-load acceptance remain open.

## 1. Data sets

| Data set | Content | Where / lifetime | Size (C6) |
|---|---|---|---|
| Competition list | up to 3 entries: active flag, primary source, optional fallback source, display names of competition and club | settings (NVS), persistent | part of the settings record ([CONFIGURATION.md](CONFIGURATION.md)) |
| Fixture pairs | up to 8 links of a primary and a fallback match id | settings (NVS), persistent | part of the settings record |
| Snapshot | matches, table rows, events and state of all competitions | RAM: 2 published slots + 1 route scratch, rebuilt every round, lost on reboot | 27,200 B each, 81,504 B together |
| Selection page | one page of a provider's competition or club list for the setup wizard | RAM, per request | 64 entries of 131 B |
| Display view model | what one screen shows, copied from the snapshot | RAM | 1,420 B |
| REST overview | own match and per-competition state, computed from the snapshot | per request | – |
| Core data (competition, teams, short names, rounds) | **does not exist**: names arrive with every match response, nothing is stored | – | – |

## 2. Definitions

| Term | Meaning |
|---|---|
| Competition | one entry of the competition list: a league, cup or tournament at one provider (plus optional fallback). Matches refer to it by list position (field `route`). |
| Tracked club / own club | the club id of the competition entry's source. Every entry needs one today. |
| Own match | a match of the tracked club. The Own match screen shows the one in its window, else the one with the kickoff nearest to now. |
| Table competition | the first active competition of the list; only it has a table. |
| Round | the provider's matchday or cup round (number and/or label). |
| Matchday window | from the first kickoff through the last finish of a round, plus configurable offsets; it sets the polling cadence. |
| Konferenz block | all retained matches of all active competitions with the same kickoff time (section 7.2). |
| Provenance | every value records its source: None (unavailable), Estimated, or the provider that supplied it. |
| Principal score | the ordinary goals of the match (after extra time when played), never including shootout goals. |

## 3. Identity and values

- A provider id is meaningful only with its provider and competition. Ids
  are at most 32 bytes; overflow or an embedded NUL is rejected.
- `Value<T>` holds a value and its source. None means unavailable; a real
  zero stays valid. Missing values are shown as `--`, never as 0.
- Kickoff and provider-change instants are UTC seconds. Fetch and
  score-change times are monotonic milliseconds, so clock corrections do not
  reorder freshness. A finished time observed locally is marked estimated.
- Display names hold 64 bytes, short names 24; displayed prefixes end at
  complete UTF-8 characters; unknown glyphs use the font fallback.
- Numbers accept integral JSON numbers and decimal strings; booleans, null,
  fractions, overflow and out-of-range values are rejected. Goals 0–99,
  minutes 0–240, added minutes 0–120, table points and difference ±10,000.
- Teams, names, scores and events are copied into the snapshot; nothing
  borrows JSON storage.

## 4. Records and fields

Columns: **Req.** = the record is dropped without the field.
Provider columns: ✓ filled, (✓) partly, ✗ never, E = estimated by the
device. OL = OpenLigaDB, ES = ESPN, AF = API-Football, FD =
football-data.org. Mapping details: [DATA_FLOW.md](DATA_FLOW.md).

### 4.1 Competition entry (settings)

| Field | Meaning | Req. | Used for |
|---|---|---|---|
| `active` | entry is fetched and shown | – | all screens; the first active entry is the table competition |
| `primary` / `fallback` | provider, competition id, season (empty = current), club id | primary competition and club id | requests; club id marks own matches |
| `fallback_active` | use the fallback source | – | combining (DATA_FLOW section 6) |
| `competition_name` | display name | – | Own match caption, Table title, start screen |
| `team_name` | club display name | – | start screen title |

### 4.2 Team (124 B)

| Field | Meaning | Req. | OL | ES | AF | FD | Shown |
|---|---|---|---|---|---|---|---|
| `provider`, `id` | identity | id | ✓ | ✓ | ✓ | ✓ | – (own-club test) |
| `name` | full name (64 B) | – | ✓ | ✓ | ✓ | ✓ | when no short name |
| `short_name` | short name (24 B); full name when the provider has none | – | ✓ `shortName` | ✓ `shortDisplayName` (partly English) | ✗ → full name | ✓ `shortName` (doc) | Own match, Konferenz, Table |

### 4.3 Match (456 B)

| Field | Meaning | Req. | OL | ES | AF | FD | Shown / used |
|---|---|---|---|---|---|---|---|
| `provider`, `route`, `id` | identity: provider, competition position, match id | id | ✓ | ✓ | ✓ | ✓ | fixture pairs, reconciliation |
| `round` | round number | – | ✓ | ✗ | ✗ | ✓ `matchday` | Own match caption "N. Spieltag" when no label; window grouping |
| `round_label` | round text | – | ✓ `groupName` | ✗ | ✓ `league.round` | ✓ `stage` | Own match caption (preferred over the number) |
| `home`, `away` | teams | id | ✓ | ✓ | ✓ | ✓ | Own match, Konferenz, REST |
| `kickoff` | UTC kickoff | – | ✓ | ✓ | ✓ | ✓ | windows, Konferenz blocks, time before kickoff, date pill, countdown, REST |
| `finished_at` | first time seen finished (estimated) | – | E | E | E | E | Konferenz block end |
| `provider_changed` | provider's last change time | – | ✓ (only with time zone) | ✗ | ✗ | ✓ | **not used** |
| `phase` + source | Unknown … Abandoned (section 5) | – | (✓) finished flag, otherwise E | ✓ | ✓ | ✓ | pill, windows, Konferenz order and centre, rim, REST |
| `minute` | match minute | – | E (`~`) | ✓ | ✓ | ✗ | pill, rim, REST |
| `principal` | ordinary goals | – | ✓ | ✓ | ✓ | ✓ | score centre, Konferenz, REST, goal flash |
| `halftime` | half-time score | – | ✓ | ✗ | ✓ | ✓ | **not shown** |
| `regulation` | score after 90 minutes | – | ✓ | ✗ | ✓ | ✓ | **not shown** |
| `extra_time` | score after extra time | – | (✓) incomplete at times | ✗ | ✓ | ✓ | **not shown** (feeds `principal`) |
| `shootout` | shootout tally | – | ✓ (derived) | ✓ | ✓ | ✓ | pill "i.E. 4 : 2" |
| `events[12]`, `event_count` | indices into the event pool | – | own club only | own club only | own club only | ✗ | Own match highlight line |
| `events_truncated` | more than 12 events or pool full | – | – | – | – | – | disables removal detection; **not shown** |
| `own` | match of the tracked club | – | ✓ | ✓ | ✓ | ✓ | accent colour, own match choice |
| `score_changed_ms` | last principal score change | – | – | – | – | – | Konferenz goal flash (120 s), One-page order |
| `fetched_ms` | fetch time of its competition | – | – | – | – | – | **not used** (the snapshot time is used instead) |

### 4.4 MatchEvent (224 B)

| Field | Meaning | OL | ES | AF | FD | Shown |
|---|---|---|---|---|---|---|
| `type` | Goal, OwnGoal, PenaltyGoal, PenaltyMissed, Yellow, YellowRed, Red, Substitution, Injury, VAR | goals only | goals, cards, (substitution, injury, VAR if supplied) | all | ✗ | highlight line label |
| `minute` | minute | (✓) | ✓ | ✓ | ✗ | highlight line |
| `added` | added minutes | ✗ | ✗ | ✓ | ✗ | **not shown** |
| `actor_team`, `beneficiary` | team that acted / that profits (own goal) | derived | ✓ | ✓ | ✗ | **not shown** |
| `player` | player (substitution at ESPN: "A / B") | ✓ | ✓ | ✓ | ✗ | highlight line |
| `player_in`, `player_out` | substitution direction | ✗ | ✗ | ✓ | ✗ | highlight "out > in" |
| `detail` | provider text | ✗ | ✓ | ✓ | ✗ | **not shown** |
| `after` | score after the event | ✓ | ✗ | ✗ | ✗ | merge, identity; **not shown** |
| `shootout` | shootout kick | ✗ | ✓ | ✓ | ✗ | label "Elfmeterschießen" |
| `identity`, `revision`, `removed` | supplied id or hash; change counter; withdrawn | – | – | – | – | removed events are hidden |

### 4.5 Standing (162 B)

| Field | OL | ES | AF | FD | Shown |
|---|---|---|---|---|---|
| `team` | ✓ | ✓ | ✓ | ✓ | Table name |
| `position` | E (row order) | ✓ | ✓ | ✓ | Table "#" |
| `played` | ✓ | ✓ | ✓ | ✓ | Table "Sp" |
| `points` | ✓ | ✓ | ✓ | ✓ | Table "Pkt" |
| `difference` | ✓ | ✓ | ✓ | ✓ | Table "Diff" |
| `won`, `drawn`, `lost`, `goals_for`, `goals_against` | ✓ | ✓ | ✓ | ✓ | **not shown** |
| `own` | – | – | – | – | Table accent row, scroll position |

### 4.6 Snapshot state

| Field | Meaning | Shown |
|---|---|---|
| `match_count`, `table_count`, `event_count` | used places | – |
| `generation`, `revision` | settings generation and publication counter | – |
| `fetched_ms`, `route_fetched_ms[]` | oldest and per-competition fetch time | status band "Stand HH:MM", REST `last_update_ms` |
| `stale` | a competition shows earlier data, or age > 15 minutes | status band, REST |
| `retry_ms` | next attempt after a failure | "Keine Daten" card, REST |
| `error`, `error_cause`, `error_routes` | first failure text and cause, failed competitions | status band, cards, REST |
| `routing_notice` | fallback in use or fallback failed | REST only |
| `matches_truncated` | not every match fitted | REST / Web UI |
| `configured` | at least one active competition | setup card, stock image |

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
deterministic hash; a revision hash tracks score, detail and player changes.
Events missing from a complete later list become one-publication removal
records. The first fetch, a reconnect, a provider settings change and a
clock step establish a baseline and do not replay historical goals. The
32-bit hashes are bounded; collision and correction fixtures still need
tests.

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
- Highlight line: the retained events of this match, rotating every five
  seconds, without removed ones. With OpenLigaDB only goals appear; with
  football-data.org none.
- Rim: progress from the minute and phase.

### 7.2 Konferenz

A block is every retained match of every active competition with the same
kickoff time; postponed, cancelled and abandoned matches and matches without
a kickoff never take part. A block's matches are shown from kickoff minus the
before offset until the block ends (the latest expected end of its matches
plus the after offset) or the next block's before offset starts, whichever is
first. A match that overruns into the next block (it finished after that
start, or is still running) stays until its own end plus the after offset.
The expected end is the final whistle when known; a provider-reported
running phase allows up to 240 minutes after kickoff; otherwise 115 minutes
after kickoff are assumed. A match first seen as finished without having
been seen running gets an estimated final whistle of the earlier of now and
kickoff plus 115 minutes. With "show own match for the whole matchday" on,
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

Only the table competition (first active entry). Title: its display name.
Columns position, name, played, difference, points; columns are dropped from
the right when the name would get too short. The rows around the own club are
shown; all accepted rows (up to 32) are reachable by scrolling. Without a
table: a card.

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
| Competitions | 3 (`kCompetitionMax`) | – |
| Matches | 32 per snapshot | all competitions |
| Table rows | 32 per snapshot | the table competition only |
| Events | 32 per snapshot, 12 per match | own matches of all competitions |
| Fixture pairs | 8 | all competitions |
| Selection page | 64 entries | – |
| Kept matches | own ±7 days; others: OpenLigaDB current round, otherwise ±36 hours | – |

Overflow handling (eviction of the farthest other match, truncation flags,
table overflow) is described in [DATA_FLOW.md](DATA_FLOW.md).

## 9. Data needed but not in the model

| Data | Status |
|---|---|
| Competition kind (league / cup / tournament), list of rounds | not stored |
| Stored team list, own short names, crest reference | not stored; names come with every match response |
| Whistles (kick-off, half-time, 2nd half, full time, extra time) | only derived from the phase |
| Delays | not in the model |
| Tables of further competitions, group tables | only one table (table competition) |

## 10. Known gaps at a glance

**Stored but never shown or used:** `halftime`, `regulation`,
`extra_time` (only feed the principal score and fallback filling),
`provider_changed`, `Match.fetched_ms`, event `added`, `detail`,
`actor_team`, `beneficiary`, `after`, table `won`, `drawn`, `lost`,
`goals_for`, `goals_against`, `events_truncated` (on screen),
`routing_notice` (on screen).

**Provider-dependent display differences:**

- ESPN: no round, so no round in the Own match caption and matchday grouping
  by UTC date; substitutions without direction ("A / B"); no half-time or
  extra-time scores.
- OpenLigaDB: no minute (estimated), no cards or substitutions; extra-time
  result can be missing; table position from row order; 3. Liga and
  Regionalliga goal data incomplete.
- API-Football: round only as provider text, no short names.
- football-data.org: caption shows `stage` text instead of "N. Spieltag"
  when `stage` is supplied (provider value format not verified), no events.

**Structural limits against the target use (several leagues, national
team, competitions without an own club):**

- At most 3 competitions.
- Every competition needs a club; a competition without an own club is not
  possible.
- Only one table (first active competition).
- All competitions share 32 matches; full Bundesliga + 2. + 3. Liga
  matchdays alone are 28 matches.
- Events only for the own club; Konferenz matches have scores only.
- No per-competition choice of screens; no matchday screen.
