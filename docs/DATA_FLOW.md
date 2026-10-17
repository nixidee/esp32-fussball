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


The [common screen policy](UI.md#planned-screen-system-redesign) now derives
content demand and phase classification. Every active competition retains fixture
and live classification data; current-phase enabled entries add Table and exact
ESPN Single-summary demand, including manual-only access. Special own priority
is separate from ownership and from an ordinary Single's exact event target.

Picture frame stops automatic demand, including continuations. DNS/TLS reserves
no provider request; the first nonblocking HTTP write checks both copied settings
generations and serializes admission with publication. Already admitted requests
finish across policy-only changes. Explicit setup queries bypass frame only,
retain all request limits, and never authorize a general refresh on frame exit.

Ordinary row names/events are staged through a complete response, then committed
only for the final selected identity. Two bounded candidates and duplicate-id
tracking retain1192B metadata (+1188B static versus the response pointer); their
24 events use2016B transient heap, allocated only at the first body callback
after the816B durable pre-send fence has released. Duplicate/overflow/allocation/final-target
mismatch is visibly marked through existing loss notices; no ordinary match is
relabeled as owned. A timing-boundary selection change can require a later full
response to fill the newly selected detail target. Existing own-bank capacity and
provider limitations remain unchanged.

## 1. Pipeline

1. **Settings** supply the competition list (up to eight entries, own
   competition record), each with exactly one source (provider, competition,
   season, club), and the per-provider and global limits ([CONFIGURATION.md](CONFIGURATION.md)).
2. **Request scheduler:** one provider task asks which job runs now — one
   data type of one competition — or when to ask again
   (section 2).
3. **Request plan:** the job becomes one or more provider requests with
   their URLs (section 3).
4. **Provider client:** one HTTPS request at a time for all providers, after
   the limits and the daily caps admitted it (sections 2 and 8).
5. **Streaming:** the JSON body is parsed element by element; each element
   is mapped (section 5) and stored at once (section 4).
6. **Match store:** keeps the matches, tables, teams and own-match events of
   all competitions (section 6).
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
- **Intervals per data type** (fixtures 8 h, table 4 h, live 1 min, live table 10 min, selected-summary events 10 min; 10 s–7 days): the
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
  only that provider for the first day with a valid clock and is replaced
  by a fresh counter on the next day, without waiting for a request. A
  provider with cap 0 has no record and causes no storage write. The
  settings reset erases the records.

### Request scheduler

The request scheduler (`request_scheduler.h`) is pure logic; the provider
worker asks it which request runs now or when to ask again, so nothing
waits in a loop.

- **Jobs**: one per competition slot (8) and data type (5). Fixtures and
  table are due one interval after their
  last start. Live data and the live table are due only while a match of the
  competition is in its live window; summary events use the exact eligible
  selected Single identity and its bounded/held window, independently of the
  owned flag. Entering/changing the selected target makes it due at once. The live table and
  the events get one more request after a window ends, while the end is less
  than one interval ago in real time (final table, last events; a window
  that ended before boot counts with its real end). The table and the live
  table of one competition are the same request: a start of either counts for
  both.
- **Data types per competition** follow its screen switches: fixtures for
  any switched-on screen; live data for Konferenz, Matchday or
  Own match (the latter only with a club); table and live table for Table;
  own-match events (ESPN source with a club and Own match on). A competition
  with no screen switched on requests nothing.
- **Live window** of a match: from 30 minutes before kickoff to the final
  whistle when finished, to kickoff + 240 minutes while the provider reports
  it running or an earlier live observation justifies a reported suspension,
  else to kickoff + 110 minutes. Without kickoff, a confirmed live match or
  justified suspension starts at its first live observation and keeps the
  240-minute hard bound. An unjustified suspension has no request window.
  A finished match without kickoff retains its final-whistle boundary for
  final table/events requests. No kickoff time is invented. Postponed,
  cancelled and abandoned matches have none. Overlapping windows merge by
  their actual bounds, independent of the display store's kickoff order.
  This is the request window (`kLiveLeadMinutes`), independent of the
  configurable display window (`window_before` / `window_after`, 0–180
  minutes each). Those settings affect Own match and Konferenz selection,
  so a displayed match need not be inside its live request window. They do
  not move polling earlier or prolong it; the final table/events requests
  above remain separate.
- **Order**: due jobs run by importance: live data of a competition whose own
  match is live and the own-match events first, then live data, fixtures,
  table, live table; within a class the longest due first.
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
  fixtures, live data, own match: a class is skipped while the
  others alone need the remaining requests, otherwise its interval is
  stretched just enough (up to 255 times). A cap already used up instead
  blocks its requests until 00:00 UTC.
- **Counted start**: the scheduler counts a request (device gap, window
  limits) when its HTTP request really starts, never before. A job that
  sends nothing (its run has nothing to request) is complete for its
  interval.
- **Failures**: a sent request that fails is retried after 60 seconds,
  doubling per failure (60, 120, 240, 480 s) up to the larger of the data
  type's (possibly stretched) interval and 15 minutes; a success returns to
  the normal interval. The count is per job: one failing competition does
  not slow the other jobs of its provider; the table and the live table of
  a competition are one request and share their count. "Too many requests" also pauses
  that provider for 5 minutes. A request that is not sent or not completed
  for a local reason (device busy with another network operation, daily cap
  used up, settings or network changed during the request) is no failure:
  the job stays due; a busy device lets every provider wait one device gap.
  A request refused because the provider cannot be used (switched off, no
  key) or its request counter cannot be stored counts as a failure and
  backs off the same way, since it does not resolve by itself. A changed
  competition in a slot starts with fresh jobs.

About 0.9 KB of RAM: last start and failure count per job, the window
buckets of four providers and the global limit, and the pause times.

Selection lists are not jobs. The worker serves the owner's selection
first, one request per pass, admitted by the same device gap, pause and
window limits and counted like a job's request. While its provider's
window limit or pause holds it, the jobs of the other providers run and the
Web UI shows until when the list waits (section "Selection lists").

## 3. Request plans

The request plans (`provider_plan.h`) turn one scheduled job into provider
requests and stream each response element into the match store. A run may
take several requests; the scheduler keeps the job due until its last one,
and a failed request ends the run (the next run starts from the beginning).

| Provider | Fixtures | Table, live table | Live data |
|---|---|---|---|
| OpenLigaDB | `getmatchdata/{league}` (current round and season), `getmatchdata/{league}/{season}/{round + 1}`, with a club `getmatchesbyteamid/{club}/1/1` (only rows of this league and season) | `getbltable/{league}/{season}` | per round with a match in its live window: `getlastchangedate/{league}/{season}/{round}`, then `getmatchdata/…/{round}` only when the date changed or the last full request is one hour old (`kOpenLigaDbFullRefreshS`) |
| ESPN | `calendar/ondays` (match days of the season), then the scoreboard of every day of the current and the next matchday | `site.web.api` standings; a competition without table (cups) gives an empty table | the scoreboard of each match day with a match in its live window, else the scoreboard without date |
| API-Football | `fixtures` from today − to today + the fixture horizon | `standings` | `fixtures` of today |
| football-data.org | `{code}/matches?dateFrom&dateTo` (fixture horizon) | `{code}/standings` | `{code}/matches` of today |

- **Season:** a run that needs the season and does not know it first asks
  for it (OpenLigaDB: the current round's rows carry it; API-Football:
  `leagues?id&current=true`). A configured season is used as is. A response
  without a season fails the run. API-Football resolves the season again
  with the first fixtures run 24 hours (`kSeasonRecheckS`) after the last
  resolution, and when the clock went back before it; without a valid
  clock the known season is kept. The resolution time is kept with the
  competition's file, so a restart does not ask again.
- **Team names** come with the fixtures, table and live responses (name
  and short name per provider, see the data model); there is no separate
  team-list request.
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
  season is unknown; API-Football fixtures 2 for the run that resolves the
  season again once a day).
- **Own-match events:** OpenLigaDB and ESPN deliver goals (ESPN also
  cards) with the live data. Substitutions exist only in the ESPN match
  summary, so an ESPN source with a club and Own match on also runs the
  events job: `{slug}/summary?event={id}` for the latest own match whose
  live window has opened, once it has kicked off; only its `keyEvents`
  are read (substitutions, stored as detail events). The
  job's final request after the window ends gives the last events.
- **Table staging:** a table request fills a staging area; only a complete
  response replaces the stored table.
- **OpenLigaDB live without a known round:** the current round's matches
  (`getmatchdata/{league}`) give the round and are stored.

The planner holds 1,256 bytes of RAM (C6 build, symbol size): per
competition the season, round, change dates, match days and run positions,
plus the context of the request in progress.

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
  (cause `store_full`, E19, with what is missing).
- Events are kept only for the own club's matches (12 per match, 48 in
  all; the live and next own matches win when the pool is full).
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
- **API-Football:** no events are mapped; the provider stays limited to
  matches and tables (OQ-206).
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
is requested once more, admitted like any request, with a fresh 30 s
deadline and the first attempt's rows dropped; a second failure is
reported. Both attempts count against the provider's limits. A list never
waits inside the request: while the provider's window limit or a "too many
requests" pause holds it, the selection stays pending, the other providers'
jobs run, and `/api/v1/selection` reports the wait (`wait` = `limit` or
`retry`, `wait_s` seconds). While another network operation holds the
device, the list is retried after the device gap for at most 30 seconds. Once the page is full and one
more matching entry is known, the device stops reading the response and
closes the connection, so a long provider list (for example ESPN's league
list of about 0.6 MB) is not downloaded in full for every page. A failed
request is logged with the stage it reached (`dns`, `tcp`, `tls` or `http`)
and the socket's pending error.

## 6. One source per competition

Each competition has exactly one source: one provider with its competition,
season and club. Providers are not combined: there is no second source of a
competition, no fixture pairing between providers and no filling of values
from another provider. To follow the same league at two providers, the owner
adds it twice as two competitions; each is requested, stored and shown on
its own.

## 7. Store, publication and readers

### Failures

A request that fails ends its job's run; the job is retried with the
growing backoff of section 2 (60 s doubling up to the larger of its interval
and 15 minutes), five minutes after "too many requests". What the store holds stays: a failed competition keeps its
matches, table and events. A request that failed, of any data type,
marks its competition failed until its next successful request: the data are stale and carry the error, its cause and the next
scheduled request (`retry_ms`).

### Changes

- A save that changes a competition's source, removes or pauses it drops
  its stored data and starts its jobs afresh, and its competition file goes
  with the next sweep; the other competitions keep data and due times. A
  changed provider key or ESPN opt-in does the same for every competition
  that uses that provider; a changed demo mode clears the whole store and
  every job.
- A save itself requests nothing else: changed limits, intervals or match
  windows apply with the next decision, other settings do not touch the
  jobs.
- A refresh (the Web UI's refresh action only) makes every job due at once;
  counted requests, window limits, pauses and the device gap stay.
- A clock step re-runs the maintenance (expiry and OpenLigaDB estimates) at
  once. Neither a clock step nor a network reconnect requests again: jobs
  that became due meanwhile run anyway.
- Every request carries the provider-data settings generation and the
  network epoch; a change to that generation or the network during a
  request stops it and its run. A cosmetic settings save does not change
  the provider-data generation and does not cancel already-started transport.
  Before the first HTTP send, both generations must still match the worker's
  coherent snapshot. A policy or request-limit save therefore refuses an
  obsolete unsent request without discarding valid cached data. The setup
  wizard bypasses the frame pause only, so it cannot send using limits saved
  before a concurrent change either. Its current requests remain available
  when frame mode is already active.

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
- **Seed:** with the first valid clock, the fixtures and table
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

The store (26,712 B) is one static object in BSS. A reader runs under the
repository mutex and copies what it needs into its own fixed model; the
display presenter owns one static 1,880-byte view model. A full model copy
would require separate storage, including stack space if made locally.
Reader callbacks do not fetch provider data or perform filesystem or LVGL
work. The presenter can acquire the time-service mutex for local-time
conversion and the settings mutex for competition lookups while holding
the repository mutex. These acquisitions wait with `portMAX_DELAY`, so the
reader is not a nonblocking path. Its total lock hold, including those
waits under concurrent service work, remains to be measured. Rendering
runs after the reader returns.

### Views

The screens and the Web UI read the store through `store_views.h`. The views keep no data; they
return store indices in fixed lists on the caller's stack (226 B for a
match list, 232 B for a table on the C6).

- **Shown source:** every active competition shows the matches of its
  source; its club id marks the own matches.
- **Konferenz:** the membership and paging rules of
  [DATA_MODEL.md](DATA_MODEL.md#72-konferenz) (block of kickoffs, same
  round, window before and after the match, own match pinned), applied to
  the shown matches. A match's expected end is the one the
  request scheduler uses for its live window: the final whistle when
  known, up to 240 minutes after kickoff while the provider reports it
  running, else 110 minutes after kickoff; never later than 240 minutes
  after kickoff.
- **Single and phase:** the common canonical selector resolves focused own,
  unique ordinary, held result or next preview. Owned flags and special-own
  treatment are independent. Current-phase enabled/manual rules derive provider
  demand; no old screen mask or independent resolver gates content.
- **Names/events:** selected ordinary and own identities share full names and
  bounded event banks. Owner custom names take priority. Removal markers stay
  hidden; regular time precedes shootout, then minute/added time/provider order.
  Truncation is visible and another match's successful update cannot clear it.
- **Table:** automatic pages centre around the selected club where present;
  held manual paging reaches all stored rows. Visit time supplies page timing.
- **Matchday:** canonical verified rounds include known rescheduled fixtures
  outside the date cluster. Held/current/next round navigation is separate from
  the global phase's local-date grouping.
- **Conference:** one competition per entry, complete chronological paging or
  One-page activity ranking. Shared match capacity is at least2; own pin counts
  toward capacity and leaves remaining places for other fixtures. Bounded
  redistribution keeps page count, places larger pages first and creates no
  empty remainder page. Manual anchors use actual page boundaries.
- **Demo:** seven controllable canonical scenarios (own, ordinary, conference,
  cup, rounds, break, empty), with ordered steps and a separate clock. First two
  saved competition identities supply order and screen rules through a runtime
  projection. Missing saved associations use common rules and are exposed as
  unbound. Synthetic sources are never saved; no real cache sweep/write or
  provider HTTP occurs. Exit reloads retained real data. Store/routes/clock/
  bindings/phase proof publish atomically; command acknowledgement is separate
  from completion. Fixture identities/names/events use recorded public data;
  timings, intermediate states and table values are explicitly simulated.

## 8. Request bounds

- HTTPS verifies the common-root IDF CA bundle and needs a valid clock. No
  redirect is followed.
- One heavy-operation token serializes TLS and parsing with uploads, image
  validation/deletion, complete screen-policy HTTP and OTA. Ordinary jobs keep
  it through detail commit/release; discovery takes it before state allocation
  and releases after proof persistence/cleanup. A routine LVGL JPEG redraw can still overlap
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


## Schedule evidence and provider protocol — 2026-10-17

The HTTP client and host replay use one streaming parser kernel and unchanged
body/24-KiB allocator limits. Optional anchored metadata and name observers share
the existing filter/value lifetime; no complete body is retained. A bounded
syntax guard also validates skipped tokens. Parent array counts and wrapper
shape prevent metadata from unrelated league objects being combined. Callers
must stage and validate metadata, then commit only after a complete successful
response with all required fields, types, uniqueness and parent identity proven.
An early wizard stop is successful list pagination, never a complete schedule.

The worker owns one compact evidence array independently of the224display
matches. Complete spans require source/season identity, semantic metadata,
complete framing/counts/pages, freshness and actual neighbouring kickoffs.
Normal rows are checked before store admission/eviction, using the planner's
same source filter. Display absence and calendar day markers prove no gap.

`schedule.bin` stores the explicit816B CRC record; its temporary replacement
also needs physical filesystem headroom. It counts toward the existing48KiB
football quota and preserves the image reserve. Corrupt file bytes still count
until replaced or removed. Before any match/discovery first-send, a durable
conservative version fences that source's existing proof. A reset during an
unfinished response therefore reloads Unknown. Publication remains conservative
until the safe replacement succeeds. Optional coverage write failures do not
starve established competition/short-name persistence.

The authoritative48B discovery guard is in the existing budget NVS namespace.
The file contains a non-authoritative guard snapshot. Formatting/missing files,
reboot or midnight cannot replenish the16extra requests of an active run.
Ordered source/incarnation/access fingerprints reject changed routes or access
after reboot. Changed data settings retire an old run without granting another
same-day run. At most one new run starts per UTC day and at most two seasons
are considered; ordinary due/live jobs, setup and provider limits retain priority.

OpenLigaDB full-season proof is limited to actual outer kickoffs of a complete
validated response; an oversized season remains Unknown. Keyed date ranges
require matching filters, counts and complete pages. ESPN calendar labels do
not define UTC kickoff buckets, so currently replayed league/cup replies remain
Partial instead of inventing coverage. Keyed-provider and full hardware coverage
acceptance remain open.

The worker reads its model, competition order and both version markers in one
short settings-service snapshot. The optional derived eligibility uses exactly
that route order; later saves are detected by the existing generation/admission
checks. No complete screen policy is copied into provider-owned storage.

### Selected summary cadence checks

The maintained host tests exercise actual selection→Planner demand→Scheduler→
summary URL for both own and ordinary selected Singles: initial request, exactly
one final request within its interval, and no final request after that interval.
A zero target never infers demand from an owned flag; a job naming a different
currently selected match is rejected by the Planner. The unknown-kickoff case
also runs after a prior request, so it cannot pass solely through initial-target
admission. These synthetic boundary checks do not establish real provider
update speed or device transport acceptance.
