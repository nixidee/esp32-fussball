# Data flow

How the firmware assembles football data: which requests it sends and when,
how it filters, maps, combines, stores and publishes the responses, and the
bounds every request must fit. What is stored and shown is described in
[DATA_MODEL.md](DATA_MODEL.md); provider facts, limits and endpoints in
[DATA_PROVIDERS.md](DATA_PROVIDERS.md).

Implementation: `components/football/` (`football_service.cpp` worker,
`request_scheduler`, `provider_plan`, `store_mappers`, `match_store`,
`store_views`, `provider_client`, `request_budget`, `budget_ledger`). The C6
build compiles and the scheduler, plans, mappers, store and views are
host-tested. Firmware requests and keyed-provider coverage have not been
accepted on the device.

## 1. Pipeline

1. **Settings** supply the competition list (up to eight entries, own
   competition record), each with a primary source and an optional fallback
   source of another provider, the fixture pairs and the per-provider and
   global limits ([CONFIGURATION.md](CONFIGURATION.md)).
2. **Request scheduler:** one provider task asks which job runs now — one
   data type of one source of one competition — or when to ask again
   (section 2).
3. **Request plan:** the job becomes one or more provider requests with
   their URLs (section 3).
4. **Provider client:** one HTTPS request at a time for all providers, after
   the limits and the daily caps admitted it (sections 2 and 8).
5. **Streaming:** the JSON body is parsed element by element; each element
   is mapped (section 5) and stored at once (section 4).
6. **Match store:** keeps the matches, tables, teams and own-match events of
   all competitions and combines a paired fallback into the primary match
   (section 6).
7. **Publication:** after every request the worker derives the shown
   sources, the error and stale state and increments the revision; readers
   (display presenter, REST status) read the store under a short mutex
   (section 7).

## 2. Scheduling and limits

There are no rounds: every data type of every source has its own due time.
Between decisions the worker sleeps until the next due time, at most one
minute (`kStoreMaintenanceS`), and wakes earlier for a refresh, a settings
save, a selection list, a clock step, a network change or an update test.

The REST status reports the next request and the reason of the wait
(`next_request_ms`, `next_request_reason`; [API.md](API.md)).

### Limits per provider and global

Each provider has a request plan (defaults in the order OpenLigaDB /
API-Football / ESPN / football-data.org):

- **Window limit** (60 per 60 s / off / off / 10 per 60 s; requests
  0–65535 per window 0–43200 s; 0 = no limit): shared by all requests of
  that provider, including selection lists, the fixture picker and multiple
  competitions; counted exactly (below).
- **Device gap**: 2 seconds between the starts of any two requests of the
  device, independent of the limits.
- **Intervals per data type** (core data 1 day, fixtures 8 h, table 4 h,
  live 1 min, live table 10 min, own-match events 10 min; 10 s–7 days): the
  due times of the jobs.
- **Global limit** over all providers (daily cap and window limit, default
  off): the window limit is counted like a provider's; the daily cap has its
  own ledger record (key `global`) and is checked before a provider's
  counter takes the request.
- **Daily cap** (0 / 100 / 0 / 0 requests, 0–65535; 0 = no cap): every
  attempt, including selections, counts before TLS starts. A durable 12-byte
  NVS record per provider reserves up to eight requests at once; a restart
  loses unused reserved requests and cannot replenish the day, and moving the
  clock backward cannot reopen a spent day. The day changes at 00:00 UTC. A
  storage failure refuses that provider's request; a damaged record blocks
  only that provider for the current day. A provider with cap 0 has no
  record and causes no storage write. The settings reset erases the records.

### Request scheduler

The request scheduler (`request_scheduler.h`) is pure logic; the provider
worker asks it which request runs now or when to ask again, so nothing
waits in a loop.

- **Jobs**: one per competition slot (8), source (primary, fallback) and data
  type (6). Core data, fixtures and table are due one interval after their
  last start. Live data and the live table are due only while a match of the
  competition is in its live window, own-match events only while the own
  match is; entering a window makes them due at once. The live table and
  the events get one more request after a window ends, while the end is less
  than one interval ago in real time (final table, last events; a window
  that ended before boot counts with its real end). The table and the live
  table of one source are the same request: a start of either counts for
  both.
- **Data types per competition** follow its screen switches: core data and
  fixtures for any switched-on screen; live data for Konferenz, Matchday or
  Own match (the latter only with a club); table and live table for Table;
  own-match events (ESPN primary with a club and Own match on). A fallback
  source requests at most the types of its competition. A competition with
  no screen switched on requests nothing.
- **Live window** of a match: from 30 minutes before kickoff to the final
  whistle when finished, to kickoff + 240 minutes while the provider reports
  it running, else to kickoff + 110 minutes. Postponed, cancelled and
  abandoned matches have none. Overlapping windows of a competition merge.
- **Order**: due jobs run by importance: live data of a competition whose own
  match is live and the own-match events first, then live data, fixtures,
  core data, table, live table; within a class the longest due first.
- **Limits**: a request starts only after the 2-second device gap, outside a
  provider pause, with requests left under the provider's and the global
  daily cap, and within the provider's and the global window limit. A window
  limit of N requests in W seconds is counted in 32 time buckets of
  W ÷ 31 seconds (rounded up); a bucket counts for the whole window while any
  part of it is inside, so N is never exceeded and at most one bucket width
  of the allowance stays unused. The counts are kept in RAM: a restart or a
  changed limit starts them empty. A job whose provider must wait does not
  hold back a due job of another provider.
- **Degradation**: when the requests expected until 00:00 UTC exceed what a
  daily cap has left, data types give way in the order live table, table,
  core data, fixtures, live data, own match: a class is skipped while the
  others alone need the remaining requests, otherwise its interval is
  stretched just enough (up to 255 times). A cap already used up instead
  blocks its requests until 00:00 UTC.
- **Failures**: a failed request is retried after 60 seconds, doubling up to
  the data type's (possibly stretched) interval; "too many requests" also
  pauses that provider for 5 minutes. A changed competition in a slot starts
  with fresh jobs.

About 0.9 KB of RAM: last start and failure count per job, the window
buckets of four providers and the global limit, and the pause times.

Selection lists and the fixture picker are not jobs: they wait for the
same device gap, pause and window limits and are counted like a job's
request.

## 3. Request plans

The request plans (`provider_plan.h`) turn one scheduled job into provider
requests and stream each response element into the match store. A run may
take several requests; the scheduler keeps the job due until its last one,
and a failed request ends the run (the next run starts from the beginning).

| Provider | Core data | Fixtures | Table, live table | Live data |
|---|---|---|---|---|
| OpenLigaDB | `getavailableteams/{league}/{season}` | `getmatchdata/{league}` (current round and season), `getmatchdata/{league}/{season}/{round + 1}`, with a club `getmatchesbyteamid/{club}/1/1` (only rows of this league and season) | `getbltable/{league}/{season}` | per round with a match in its live window: `getlastchangedate/{league}/{season}/{round}`, then `getmatchdata/…/{round}` only when the date changed or the last full request is one hour old (`kOpenLigaDbFullRefreshS`) |
| ESPN | `{slug}/teams` | `calendar/ondays` (match days of the season), then the scoreboard of every day of the current and the next matchday | `site.web.api` standings; a competition without table (cups) gives an empty table | the scoreboard of each match day with a match in its live window, else the scoreboard without date |
| API-Football | `teams?league&season` | `fixtures` from today − to today + the fixture horizon | `standings` | `fixtures` of today |
| football-data.org | `{code}/teams` | `{code}/matches?dateFrom&dateTo` (fixture horizon) | `{code}/standings` | `{code}/matches` of today |

- **Season:** a run that needs the season and does not know it first asks
  for it (OpenLigaDB: the current round's rows carry it; API-Football:
  `leagues?id&current=true`). A configured season is used as is. A response
  without a season fails the run. API-Football resolves the season again
  with every core-data run.
- **Matchdays (ESPN):** match days less than the matchday gap apart form one
  matchday (`kMatchdayGapHours`, 48 h); the current matchday is the first
  whose last day has not ended.
  At most 8 days are kept. While streaming, days of ended matchdays are
  dropped and the scan stops after the next matchday.
- **Change dates (OpenLigaDB):** the change date of up to three rounds is
  kept as a hash with the time of the last full request.
- **Costs:** the scheduler's daily estimate multiplies the run frequency by
  the requests of one run (OpenLigaDB fixtures 2, with a club 3; live 2;
  ESPN fixtures 1 + the kept days, 7 while unknown; one more while the
  season is unknown).
- **Own-match events:** OpenLigaDB and ESPN deliver goals (ESPN also
  cards) with the live data. Substitutions exist only in the ESPN match
  summary, so an ESPN primary with a club and Own match on also runs the
  events job: `{slug}/summary?event={id}` for the latest own match whose
  live window has opened, once it has kicked off; only its `keyEvents`
  are read (substitutions, stored as the primary's detail events). The
  job's final request after the window ends gives the last events.
- **Table staging:** a table request fills a staging area; only a complete
  response replaces the stored table.
- **OpenLigaDB live without a known round:** the current round's matches
  (`getmatchdata/{league}`) give the round and are stored.
- **Fallback source:** see section 6.

About 2.4 KB of RAM (host measurement): per competition and source the
season, round, change dates, match days, run positions and route state,
the fixture pairs of all competitions, plus the context of the request in
progress.

## 4. Filtering while streaming

- The request plans ask only for what is kept: OpenLigaDB the current and
  the next round and, with a club, the club's matches one week back and
  ahead; ESPN the
  days of the current and the next matchday; API-Football and
  football-data.org the fixture horizon (7 days) and today.
- OpenLigaDB club rows of another league or season are dropped.
- Every element is stored at once; a duplicate match id of the same
  competition and provider updates the stored match.
- A full store makes room by the rule of
  [DATA_MODEL.md](DATA_MODEL.md#46-store-rules) and notes the competition
  (`matches_truncated`).
- Events are kept only for the own club's matches (12 per match).
- Discarded bytes still count toward the transport limits (section 8).

## 5. Mapping rules

Definitions of every field: [DATA_MODEL.md](DATA_MODEL.md). Unknown or
missing values stay unavailable; nothing is fabricated.

### Teams

Short name: OpenLigaDB `shortName`, ESPN `shortDisplayName`, football-data
`shortName`; API-Football has none. An empty short name is filled with the
full name. The owner's replacement (section 7, "Kept data") is applied when
a view reads the name, not when mapping, so the store keeps the provider's
name.

### Phase and minute

- **OpenLigaDB** has only `matchIsFinished`. Otherwise phase and minute are
  estimated from the kickoff: before kickoff Scheduled; minutes 0–45 after
  kickoff Live; 46–60 HalfTime; later minutes subtract the 15-minute break.
  Estimated minutes are marked. After four hours without a confirmed final
  status the phase becomes Unknown and the window expires.
- **ESPN:** state `pre`/`in`/`post` (+ `completed`) gives Scheduled / Live /
  Finished; status names give HalfTime, ExtraTime, Penalties, Postponed
  (also SUSPENDED), Cancelled, Abandoned. The minute is the leading number of
  `displayClock` while the match runs.
- **API-Football:** `fixture.status.short` and `elapsed`.
- **football-data.org:** SCHEDULED and TIMED → Scheduled, IN_PLAY → Live,
  PAUSED → HalfTime, EXTRA_TIME → ExtraTime, PENALTY_SHOOTOUT → Penalties,
  FINISHED → Finished, SUSPENDED and POSTPONED → Postponed, CANCELLED →
  Cancelled, AWARDED → Abandoned (SUSPENDED and AWARDED follow API-Football's
  SUSP and AWD, since the documentation does not describe them). Any other
  value stays Unknown.
- No numeric status or result id is treated as a semantic constant.

### Scores

- **OpenLigaDB:** semantic `resultTypeKind`. Principal = after extra time if
  present, else after 90 minutes. The result after penalties includes the
  earlier goals; the known after-extra-time total is subtracted for the
  shootout tally. Without that base, or if the subtraction would be
  negative, the tally stays unavailable. Unknown kinds are not reassigned by
  name or order.
- **ESPN:** principal score per competitor (not before kickoff) and separate
  `shootoutScore`. No half-time, regulation or extra-time score.
- **API-Football:** `goals` (principal) and `score.halftime`, `fulltime`,
  `extratime`, `penalty`.
- **football-data.org:** `halfTime`, `regularTime`, `penalties`; extra time =
  regulation plus the additional `extraTime` goals. With `duration` REGULAR,
  `fullTime` is principal and regulation; otherwise principal = extra time,
  else regulation, so the shootout-inclusive `fullTime` is never used as
  ordinary goals.

### Round

OpenLigaDB `group.groupOrderID` (number) and `groupName` (label);
football-data `matchday` and `stage`; API-Football `league.round` (label
only); ESPN none.

### Events

- **OpenLigaDB:** goals only (goal, own goal, penalty goal) with minute,
  scorer and score after the goal; the beneficiary is derived only when
  consecutive reported scores identify a single increment.
- **ESPN:** the `details` of each received match row: goal, own goal,
  penalty goal / missed, yellow, second yellow, red, substitution, injury,
  VAR, shootout flag, minute from the clock. A substitution keeps both
  names in array order as "A / B" in its player field (cut at a whole
  character after 32 bytes); in/out stay empty because the scoreboard does
  not say who comes on. The match summary's `keyEvents` substitutions give
  player in and out (participants [in, out]) and the minute with added time;
  they are the only events read from the summary.
- **API-Football:** the mapper reads `fixtures/events` (goal types, cards,
  substitution with player in from `assist`, VAR, minute and added time),
  but no request plan asks for them.
- **football-data.org:** no events are mapped.
- A supplied event id is preferred; otherwise a deterministic hash of type,
  minute, added time, team, player, substitution and shootout identity.
  Revision hashes include score, detail and player changes.

### Table

All providers map position, played, won, drawn, lost, goals for, goals
against, difference and points. OpenLigaDB has no position; it is the row
order (marked estimated). football-data uses only the `TOTAL` table.

### Selection lists

Competition and club lists use a separate 64-entry page with a "more" flag,
not an unbounded cache. OpenLigaDB entries of other sports are skipped.
A server error (HTTP 5xx) or a failed connection is often brief, so a list
is requested once more after the device gap, with a fresh 30 s deadline and
the first attempt's rows dropped; a second failure is reported. Both
attempts count against the provider's limits. Once the page is full and one
more matching entry is known, the device stops reading the response and
closes the connection, so a long provider list (for example ESPN's league
list of about 0.6 MB) is not downloaded in full for every page. A failed
request is logged with the stage it reached (`dns`, `tcp`, `tls` or `http`)
and the socket's pending error.

## 6. Combining providers

Each competition may have one fallback source of another provider. Up to
eight explicit fixture pairs bind a primary and a fallback match id within a
configured competition. Enabling a pair certifies that identity and
home/away order were verified; names or search suggestions never certify it,
and there is no automatic cross-source matching.

The sources are combined when a response is stored, not when a screen is
built. The fallback is requested as jobs of its own (its provider's limits
apply), only while it is active:

| Fallback data | Requested while |
|---|---|
| Live data | the competition has an active fixture pair, or the primary's last match request failed |
| Fixtures | the primary's last match request (fixtures or live) failed |
| Table, live table | the primary has no table (a cup, or its last table run failed or left none); skipped while the store holds the primary's table |
| Core data | never: the match rows carry the team names |

- **Pair matched:** the fallback row fills the primary match as described
  in the data model (missing or estimated values only, confirmed values,
  fallback bit per field) and its events are stored as fallback events of
  the primary match. Unpaired fallback rows are dropped.
- **Primary match request failed:** the competition notes the fallback
  route; every fallback row is stored on its own, nothing fills. The
  primary's next successful match request clears the note and removes the
  fallback rows. Which rows a screen shows during the route is decided when
  the screens are built from the store.
- **Primary without a table:** the fallback table is stored like the
  primary's; the primary's next complete table replaces it.
- **Fallback request failed:** the competition notes it; the next
  successful fallback request clears the note. The primary data stay.
- **Live rounds and days:** an ESPN fallback finds its match days from the
  kickoffs of any provider's matches of the competition (else the
  scoreboard without date); an OpenLigaDB fallback uses its current round.

## 7. Store, publication and readers

### Failures

A request that fails ends its job's run; the job is retried after 60
seconds, doubling up to its interval, five minutes after "too many
requests". What the store holds stays: a failed competition keeps its
matches, table and events. A primary request that failed, of any data
type, marks its competition failed until the next successful primary
request: the data are stale and carry the error, its cause and the next
scheduled request (`retry_ms`), unless the fallback route stands in
(primary match request failed, fallback not failed).

### Changes

- A save that changes a competition's sources, fallback switch or fixture
  pairs, removes or pauses it drops its stored data and starts its jobs
  afresh and its competition file goes with the next sweep; the other
  competitions keep data and due times. A changed provider key or ESPN
  opt-in does the same for every competition that uses that provider
  (primary or alternate source); a changed demo mode clears the whole store
  and every job.
- A save itself requests nothing else: changed limits, intervals or match
  windows apply with the next decision, other settings do not touch the
  jobs.
- A refresh (the Web UI's refresh action only) makes every job due at once;
  counted requests, window limits, pauses and the device gap stay.
- A clock step re-runs the maintenance (expiry and OpenLigaDB estimates) at
  once. Neither a clock step nor a network reconnect requests again: jobs
  that became due meanwhile run anyway.
- Every request carries the data settings generation and the network epoch;
  a save or a network change during a request stops it and its run.

### Kept data

Each active competition's data are kept in a competition file
([DATA_MODEL.md](DATA_MODEL.md#48-competition-files-and-short-names)), so
a reboot shows them without a request.

- **Write:** when the worker is idle (no job due), at most one file per
  pass: a competition with new data, at most every 10 minutes
  (`kCompetitionFileGapS`) and not while one of its matches is in its live
  window. The file is written to a temporary file and renamed, so a failed
  write keeps the previous file. A write is skipped when all football files
  together would exceed 48 KiB (`kFootballFilesBytes`, whole 4 KiB blocks
  counted), during the first boot of a firmware update, or when it would
  not leave free on the filesystem the part of the image library quota
  (320 KiB) that images do not use yet, in whole blocks, plus the 40 KiB
  image reserve: images come first, so near a full image library some
  competitions are not kept until there is room. Demo data are never
  written.
- **Load:** at boot, before the first request, the short names and the
  files of the active competitions whose sources are unchanged. Loaded data
  count as old as the file ("Stand" may lie before the boot).
- **Seed:** with the first valid clock, the core data, fixtures and table
  requests of a loaded competition are due when their interval since the
  saved completion has passed; one older than its interval runs at once.
  Live requests and competitions without a file start as without kept data.
- **Sweep:** after the boot and after every save, files of removed, paused,
  changed or never loaded competitions are deleted when the worker is idle.
- **Short names:** a change from the Web UI is written at once; a full
  table (48), budget or filesystem rejects it and keeps the previous name.
  The settings export carries them; an import replaces the whole table
  and writes the file once (a backup without short names keeps them).
- **Reset and repair:** a settings reset deletes every competition file and
  the short names. Repairing the image filesystem formats it; afterwards
  the worker writes the short names and every competition with data again.

All football file I/O holds the heavy-operation token (section 8).

### Publication

After every request and every maintenance pass the worker publishes under
the mutex: the shown source of every competition, the error summary (first
failed competition), the routing notice and a new revision. The fetch time
shown as "Stand" is the oldest last successful request over the active
competitions. Data are stale when a competition is failed (above), or
when a shown match is in its live window and its competition's last
successful match request is older than 15 minutes (`kDataStaleS`).

### Readers

The store (22,728 B) is one static object in BSS. A reader runs under the
repository mutex and copies what it needs into its own fixed model
(display view model 1,420 B); it performs no I/O, LVGL work, allocation or
blocking call there.

### Views

The screens and the Web UI read the store through `store_views.h`. The views keep no data; they
return store indices in small fixed lists on the caller's stack (largest:
194 B for a match list, 200 B for a table).

- **Shown source:** every active competition shows its primary provider's
  matches. While the competition notes the fallback route and the store
  holds fallback matches of it, it shows the fallback's matches instead and
  the own club is the fallback's team id. Other providers' rows stay hidden.
- **Konferenz:** the membership and paging rules of
  [DATA_MODEL.md](DATA_MODEL.md#72-konferenz) (block of kickoffs, same
  round, window before and after the match, own match pinned), applied to
  the shown matches. A match's expected end is the one the
  request scheduler uses for its live window: the final whistle when
  known, up to 240 minutes after kickoff while the provider reports it
  running, else 110 minutes after kickoff; never later than 240 minutes
  after kickoff.
- **Own match:** the own-match screen takes the first own match inside its
  window, else the one with the closest kickoff; the Web UI overview takes a
  running own match first, else the closest kickoff.
- **Names:** the owner's short name for the team, else the team
  directory's short name, else the own match's full name; the full name is
  kept only for own matches.
- **Screen filter:** each screen reads only the competitions whose switch
  for it is on (Own match, Konferenz, Matchday, Table).
- **Table:** the stored rows of one competition in table order with the own
  row marked. The Table screen cycles through the competitions with a
  stored table in settings order ([DATA_MODEL.md](DATA_MODEL.md#73-table)).
- **Matchday:** the first matchday of one competition among its shown
  matches with a kickoff (kickoffs less than `kMatchdayGapHours` apart), in
  kickoff order: the current matchday until it expires, then the next.
- **Events:** only for own matches; removal markers and fallback events
  hidden by the primary's own events are left out. Order: regular time
  before the shoot-out, then minute and added minute; an event without a
  minute keeps the minute of the event before it, equal keys keep the
  provider order.
- **Demo:** one running own match (2:1, minute 67, no kickoff, no goal
  highlight) and a five-row table, both stored like provider data.

## 8. Request bounds

- HTTPS verifies the common-root IDF CA bundle and needs a valid clock. No
  redirect is followed.
- One heavy-operation token serializes TLS and parsing with uploads, image
  validation/deletion and OTA. A routine LVGL JPEG redraw can still overlap
  TLS inside its fixed pool (device measurement pending).
- A 30-second monotonic deadline covers DNS, TLS connection and handshake,
  headers, framing and reads; socket waits are at most one second. One DNS
  job runs in the lwIP thread; numeric IPv4 connection keeps the hostname for
  certificate verification and SNI. The deadline is not a rate limit and was
  not sized for large bodies. Whether the 512 KiB match summary arrives within it is
  not yet measured on the device.
- Bodies: 256 KiB for the scheduled requests and the fixture picker; 512 KiB
  for the ESPN match summary (its `keyEvents` start about 300 KB in); 1 MiB
  for the selection lists of the setup wizard and their season resolution.
- Headers 8 KiB, additional wire/framing allowance 32 KiB, JSON nesting 16,
  live parser allocations 24 KiB including allocation headers (libc overhead
  additional). Elements are mapped and released one at a time; discarded
  fields still count toward byte and depth validation.
- A malformed, excessive or incomplete response fails its job (section 7);
  elements stored before the failure stay, a staged table is dropped and
  event removals are not derived from it.
- Errors pair a diagnostic text with a canonical cause; the REST API exposes
  its stable key ([API.md](API.md)).

## 9. Verification still required

Recorded fixtures must cover missing/null/real-zero values, all score
phases, duplicate/corrected/removed events, own goals, substitutions, source
conflicts, capacity overflow, UTF-8 and season rollover. Network checks
cover trickled headers and body, framing, TLS/DNS failure, 429,
cancellation, caps across reset and clock changes and configuration while a
request runs. Real keys and maximum configured data are required for
coverage and resource acceptance. SDK scheduling and cryptographic step
duration need measurement; the deadline is not a measured hard real-time
bound. None of these suites was run for this candidate.
