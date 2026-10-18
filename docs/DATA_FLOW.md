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

`components/names/` owns the immutable club catalogue, adopted-name file codec,
two referenced file versions, complete collection import/export and visible
label resolution. Its namespace remains `football::names`; the football
service never opens, deletes or activates those files. The temporary original
provider-name query stays with the existing football worker and transport.


The [common screen policy](UI.md#current-screens-and-automatic-sequence) now derives
content demand and phase classification. Every active competition retains fixture
and live classification data; current-phase enabled entries add Table and exact
ESPN Single-summary demand, including manual-only access. Special own priority
is separate from ownership and from an ordinary Single's exact event target.

Picture frame stops automatic demand, including continuations. DNS/TLS reserves
no provider request; the first nonblocking HTTP write checks both copied settings
generations and serializes admission with publication. Already admitted requests
finish across policy-only changes. Explicit setup queries bypass frame only,
retain all request limits, and never authorize a general refresh on frame exit.

Ordinary selected-match events use bounded staging and a final selected-identity
recheck before publication. Team labels live only in the shared team directory;
there is no duplicate full-name detail record. Duplicate, capacity, allocation
and target-change failures are explicit. The same shared detail/event mechanism
has protected own-match and ordinary banks; an ordinary match never becomes
owned merely because it is selected. A later full response may be needed when
the selected identity changes during acquisition.

## 1. Pipeline

1. **Settings** supply the competition list (up to eight entries, one
   AppConfig competition section), each with exactly one source (provider, competition,
   season) and a separate selected-club preference, and the per-provider and global limits ([CONFIGURATION.md](CONFIGURATION.md)).
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
  attempt, including selections, counts at the first actual HTTP transport write;
  DNS/TCP/TLS establishment alone consumes no request. A durable 12-byte
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
- **Data demand:** every active competition retains Fixtures and Live for
  phase classification, including manual-only or currently hidden sports.
  The effective current phase adds Table/LiveTable when enabled and the exact
  ESPN Single-summary target when applicable. Ownership, ordinary selection
  and special own-match request priority are separate. Picture-frame mode
  pauses automatic jobs and continuations without erasing facts.
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

Interactive selection/original-name requests share the one existing worker.
After an actual interactive send, the next runnable automatic job gets an
attempt before another interactive request. A locally refused automatic attempt
releases that turn, preventing a busy resource from starving the editor.
Blocked/budgeted/absent automatic work and frame mode do not stall interactive
queries. All requests still pass the same send-time budgets and gap. Coverage
runs behind normal runnable work. Matching query IDs and configuration capture
protect asynchronous selection; an idle-read lease releases abandoned queries.

## 3. Request plans

The Planner turns one due job into independently admitted requests. After each
request it returns control to the Scheduler, so another source can run and
actual window/day/global limits apply to every continuation. A failed request
ends its run. No request waits for a rate limit while holding heavy admission.
Provider adapters own URLs, authentication and semantic response scope.

| Provider | Fixture acquisition | Table | Live/details |
|---|---|---|---|
| OpenLigaDB | Two full-season streams: compact Index, then selected facts. Resolve an unknown season first. | `getbltable/{league}/{season}` | Relevant rounds use change-date checks and conditional full refresh; reported goals are available, minutes are estimated. |
| API-Football | Resolve current season when needed, then two season streams for Index and selected facts. | `standings` with validated league/season/groups | Current-date fixture data; no unsupported event details are invented. |
| football-data.org | Two season streams; the first resolves the actual season when current season was requested, and the second uses that same year. | `TOTAL` standings retain each group separately | Current-date fixture data; unavailable details remain unavailable. |
| ESPN | One calendar, up to16 selected day scoreboards, and two selected-club schedule streams when a club is configured. | Validated standings; complete successful empty cups clear old rows | Relevant day scoreboards and the exact selected Single summary for supported details. |

Every source retains the union of previous/current/next full round, previous
own match and next three own matches, plus relevant live/unresolved facts.
The first stream retains compact round counts/bounds and four own identities;
it does not insert an entire season into the store. The second stream admits
only that selected union. Duplicate identities share one match record. Overflow
is explicit Partial/StoreFull, never hidden eviction or proof of no match.

For explicit ESPN seasons the calendar URL follows the actual season-reference
shape `/seasons/Y/types/1/calendar/ondays`; a scoreboard `season` query alone is
not trusted because observed responses ignore it. Reported cup stages retain
stage identity, including quarter-finals spread over two weeks. A single
Regular Season calendar uses explicitly derived48-hour date clusters; these
are not reported provider rounds. Selected days are bounded to16; the
request-only calendar accepts up to366 days and reports overflow. The club
schedule is filtered by source/season/team and proves only those own fixtures,
never a complete competition round.

Season knowledge, change hashes and traversal positions are RAM-only. A reboot
reacquires them. API-Football's current-season recheck uses its configured
24-hour interval and clock-step rules. No competition file stores that state.
Team fallback names arrive with ordinary fixtures/table/live data; the explicit
full-original editor query is a separate user action, not a hidden request.

C6 compiler layout: Planner4136 B including its2088-B response-detail metadata;
SourceState232 B includes Selection132 B per source. One Index3136 B is allocated
only for the indexing request and released before fact admission. These sizes
are not device heap/stack reserves. Exact current transport limits are below.

## 4. Filtering while streaming

- The typed Reader validates complete JSON grammar even for unknown fields.
  Relevant source/season/filter/count/parent metadata must agree; a differently
  shaped or wrong-typed primary list is an error, not an empty success.
- Positive uint32 identities are exact; decimal fractions cannot round into
  an ID. A supplied team identity must fit the shared directory before a new
  fixture is admitted. Capacity failure preserves an old valid pairing and
  score together, reports StoreFull and never creates a phantom match.
- Valid incremental fixture facts can update retained identities. Only a
  complete authoritative response establishes scope, last success and pruning;
  interrupted or contradictory responses leave retained facts Partial.
- A table transaction stages up to64 rows and shared group labels. Complete
  success atomically replaces the old table, including an empty result;
  missing, invalid, interrupted or capacity-failed results retain the old one.
- Selected ordinary details are rechecked against the exact current target
  before commit. Twelve retained highlights and reported shootout attempts
  have separate reference lists in the same bounded event pool. Truncation or
  missing attempts is explicit; no attempt sequence is inferred from totals.
- Discarded fields still count toward body, depth, UTF8 and complete-document
  limits. There is no full-body DOM, insert-all/prune workaround, runtime undo
  clone, durable football file or silent store eviction.

## 5. Mapping rules

Definitions of every field: [DATA_MODEL.md](DATA_MODEL.md). Unknown or
missing values stay unavailable; nothing is fabricated.

### Teams

Provider full and short strings arrive with fixtures, table and live rows.
Field selection/fallback happens before UTF8 clipping; a nonempty short whose
first code point cannot fit3bytes stays empty, rather than changing its source
after clipping.
OpenLigaDB supplies `shortName`, ESPN `shortDisplayName`, football-data
`shortName`; API-Football has no separate short. Provider adapters validate
complete input and retain compact26/3 fallback values in the volatile directory.
No football response is persisted and no mapper holds a saved-name table.

The view copies its current IDs and provider fallbacks under the repository
lock, then releases that lock before resolving adopted fields. A bounded cache
contains only the current visible adopted name/short values and presence bits.
Cache misses use the checked names reader under heavy admission, outside LVGL
and repository locks. An absent override uses fresh provider fallback; present
empty remains empty. No automatic switch to the other language occurs. The
immutable catalogue is an editor source and never activates merely because it
is present. Complete provider originals for editor matching are a separate
explicit streamed request, not an archive reconstructed from clipped labels.

The explicit original-name query reserves the existing interactive provider
slot with its source and a correlated query ID. The same worker discovers a
missing season, observes provider windows and charges actual send attempts;
the HTTP handler holds no heavy-operation token while it waits. Decoded full
labels pass through the shared JSON writer and a single 256-byte ring to HTTP.
There is no second provider task, HTTP client or stored answer. A slow consumer
applies backpressure within the existing operation deadline, which also bounds
the transport. HTTP checks disconnects while waiting, including before the
first response byte. Cancellation stops the producer; the slot remains owned
until the worker acknowledges it. Only a complete provider response receives
the closing JSON/HTTP terminator. A partial parse or send failure closes the
response, so the editor cannot adopt a partial successful collection. The
returned originals change the draft only after explicit adoption.

### Phase and minute

- **OpenLigaDB** has only `matchIsFinished`. Otherwise phase and minute are
  estimated from the kickoff: before kickoff Scheduled; minutes 0–45 after
  kickoff Live; 46–60 HalfTime; later minutes subtract the 15-minute break.
  Estimated minutes are marked. After four hours without a confirmed final
  status the phase becomes Unknown and the window expires.
- **ESPN:** state `pre`/`in`/`post` (+ `completed`) gives Scheduled / Live /
  Finished; status names give HalfTime, ExtraTime, Penalties, Postponed
  Cancelled, Abandoned and distinct Suspended. The minute is the leading number of
  `displayClock` while the match runs.
- **API-Football:** `fixture.status.short` and `elapsed`.
- **football-data.org:** SCHEDULED and TIMED → Scheduled, IN_PLAY → Live,
  PAUSED → HalfTime, EXTRA_TIME → ExtraTime, PENALTY_SHOOTOUT → Penalties,
  FINISHED → Finished, SUSPENDED → Suspended, POSTPONED → Postponed, CANCELLED →
  Cancelled and AWARDED → Abandoned. Any other
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
only); ESPN day/stage identity is supplied by its validated calendar; derived date
groups remain distinct from reported stages.

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

All providers retain only rendered row facts: team identity, position, played,
goal difference and points. Position from OpenLigaDB row order is marked
estimated. football-data accepts `TOTAL` tables; separate groups retain separate
rankings. A complete empty table clears old rows; missing, failed or overflowed
transactions retain the old complete table and report their actual status.

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
updates only its canonical dataset status (fixtures, table or details). A
successful request for another dataset cannot erase that failure. Existing
usable facts stay visible with the matching dataset cause, last-success time
and capacity/maintenance notice; the service also reports the next request.

### Changes

- A save that changes a competition's source, removes or pauses it drops
  its RAM data and starts its jobs afresh; the other competitions keep their
  data and due times. A
  changed provider key or ESPN opt-in does the same for every competition
  that uses that provider; a changed demo mode clears the whole store and
  every job.
- A club-only edit on an unchanged source reclassifies the existing matches,
  table markers and detail priority without clearing facts or requesting again.
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
  obsolete unsent request without discarding valid retained RAM data. The setup
  wizard bypasses the frame pause only, so it cannot send using limits saved
  before a concurrent change either. Its current requests remain available
  when frame mode is already active.

### Durable settings and names; volatile football data

Settings remain in NVS. Adopted club names and ID exceptions have their own
bounded LittleFS store. Matches, tables, events, provider season/round
resolution and schedule-completeness evidence live only in RAM. The worker
starts with no restored sporting data after each boot, then acquires current
provider responses through normal readiness, request limits and retries. There
is no next-game checkpoint, competition cache, schedule file, cache sweep,
cache eviction or cache-derived scheduler seed.

Names I/O holds the shared heavy-operation token and runs outside the repository
mutex. The existing names owner checks its committed file before resolving
labels. Legacy-name conversion, short-only immediate-write APIs and full-team
projection are removed. One current common Apply adopts the complete collection;
original provider names are fetched explicitly into the browser draft only.

Explicit factory reset removes adopted names; optional filesystem formatting
also removes images. The names transaction never enumerates or deletes files
owned by other services. Request counters and discovery run/request credits
remain independently durable in NVS, so loss of volatile football data does not
replenish provider request allowances.

The full original source accompanies USB/OTA as read-only application data.
`GET /api/v1/team-names/preset` takes heavy admission before streaming JSON
through 128-byte catalogue and 512-byte HTTP buffers. It allocates no global
JSON tree or LittleFS source copy. Failed/deadline-expired sends abort without
an appended JSON error or successful final chunk. Rollback uses its running
app's matching source; user values cannot mutate it.

Normal names lookups reuse the fully checked generation via a fresh 96-byte
header receipt, then address at most 16 records in a selected key/ID checkpoint
block. There is no per-refresh full saved-club scan or resident user/legacy
collection. Provider preparation and language changes touch only the existing
240 working-team slots. Raw fallback text is available only from retained
details or current provider input; old clipped labels do not recreate missing
originals.

Names replacement is a checked transaction:

1. Hold the existing heavy admission and capture names/settings/working-identity
   revisions. Admit old plus a maximal 16-KiB candidate, explicit LittleFS
   temporary overhead, unused image quota and reserve before writing.
2. Stream/normalize the bounded candidate, fully check fields/CRC and close its
   staging handles. Staging failure preserves the committed names.
3. Open the checked projection reader before allocating its 6,990-byte packed
   label/bitmap arena. Prepare full/short labels outside the mutex. Validate
   revisions and hold the short publication fence; retain the old coherent view
   on a conflict or preparation error.
4. Recheck the committed generation and physical reserve. Atomically rename
   the checked names candidate, publish its prepared labels and applied
   language/generation together, release resources, then acknowledge the
   generation/digest. Activation failure preserves the old names. Recover
   uncertain success read-only.

The complete names file retains its existing 16-KiB ceiling. The former shared
48-KiB football-file quota and optional-cache deletion mechanism are removed.
Both adopted languages and stable identity relationships remain durable;
only the selected name26/short3 pair is resident per working team. The remaining
editor and full replacement protocol must use the same common Apply boundary,
streaming codec and generation rules; their end-to-end acceptance is still open.
Final linked resource fit, allocator/FD overhead and device heap/stack acceptance
are not established merely by the encoded file sizes or host tests.

### Publication

After every request and every maintenance pass the worker publishes under
the mutex: the shown source of every competition, the error summary (first
failed competition), the routing notice and a new revision. The fetch time
shown as "Stand" is the oldest last successful request over the active
competitions. Data are stale when a competition is failed (above), or
when a shown match is in its live window and its competition's last
successful match request is older than 15 minutes (`kDataStaleS`).

### Readers

The store is one static BSS object with a C6 compiler layout of 48,976 B
(512 matches, 256 teams). Final linked integration measurement remains a
separate gate. A reader runs under the
repository mutex and copies what it needs into its own fixed model; the
display presenter owns one static 2,044-byte view model. A full model copy
would require separate storage, including stack space if made locally.
Reader callbacks do not fetch provider data or perform filesystem or LVGL
work. The presenter receives configuration and clock projections from its caller;
it does not query services or files. Visible adopted names are resolved only
after the repository callback returns. Repository lock duration still needs
device measurement. Rendering runs after the reader returns.

### Views

The screens and the Web UI read the store through `store_views.h`. The views keep no data; they
return store indices in fixed lists on the caller's stack (1026 B for a
512-entry match list,236 B for a grouped table on the C6).

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
- **Names/events:** every working team supplies one prepared full name and
  short code. Selected ordinary and own details retain raw fallback names and
  bounded event banks. Adopted selected-language names take priority. Removal markers stay
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
  unbound. Synthetic sources are never saved and cause no provider HTTP. Exit
  starts normal provider acquisition with an empty RAM football store. Store/routes/clock/
  bindings/phase proof publish atomically; command acknowledgement is separate
  from completion. Fixture identities/names/events use recorded public data;
  timings, intermediate states and table values are explicitly simulated.
  The separate Demo automatic-next display action uses the existing versioned
  UI command FIFO: it advances the next page/image boundary, then the next
  visit. From manual output it first returns to automatic selection. It does
  not change scenario/test time or saved durations; normal timing continues.
  Controller diagnostics are one separately sampled last-bound atomic word,
  not an LCD acknowledgement or part of the football publication transaction.

## 8. Request bounds

- HTTPS verifies the common-root IDF CA bundle and needs a valid clock. No
  redirect is followed.
- One heavy-operation token serializes TLS and parsing with uploads, image
  validation/deletion, complete screen-policy HTTP and OTA. Ordinary jobs keep
  it through detail commit/release; discovery takes it before state allocation
  and releases after RAM proof publication/cleanup. A routine LVGL JPEG redraw can still overlap
  TLS inside its fixed pool (device measurement pending).
- A 30-second monotonic deadline covers DNS, TLS connection and handshake,
  headers, framing and reads; socket waits are at most one second. One DNS
  job runs in the lwIP thread; numeric IPv4 connection keeps the hostname for
  certificate verification and SNI. The deadline is not a rate limit and was
  not sized for large bodies. Whether the 512 KiB match summary arrives within it is
  not yet measured on the device.
- Bodies:256 KiB for ordinary scheduled responses;1 MiB for streamed complete
  seasons, own-fixture schedules and selection/season-resolution lists;512 KiB
  for ESPN match summaries (events can start about300 KiB into the response).
  A body's transport ceiling is not a heap allocation or a completeness proof.
- Headers8 KiB, additional wire/framing allowance32 KiB and JSON nesting16.
  One strict reader maps typed records using one bounded response union for
  unordered events or table rows. Full-season retention additionally uses one
  transient compact index, freed after scope selection. No DOM, whole-body
  buffer or whole-runtime rollback copy exists. Exact current C6 workspace
  sizes and simultaneous TLS peaks are reported separately; discarded fields
  still count toward byte/depth/full-document validation.
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
bound. These broad provider/network/device gates are separate from the
targeted current-only names codec and transaction host checks.


## Schedule evidence and provider protocol

The common strict streaming grammar validates relevant metadata and skipped
values. Typed provider consumers distinguish absent, wrong-typed, duplicate,
empty and interrupted data. Only complete successful response framing and
matching source/season/scope can establish authoritative completeness. Valid
incremental fixtures remain Partial after failure; failed responses cannot
prune retained matches or clear a table. Early selection-list pagination is
never evidence of a complete competition schedule.

The worker owns one compact evidence array independently of the512display
matches. Complete spans require source/season identity, semantic metadata,
complete framing/counts/pages, freshness and actual neighbouring kickoffs.
Normal rows are checked before store admission/eviction, using the planner's
same source filter. Display absence and calendar day markers prove no gap.

Schedule evidence is volatile. During a response the streaming flag prevents
readers from using proof that may be contradicted by rows still arriving.
Only complete successful responses can establish usable proof. A reboot starts
with Unknown evidence and requires new provider acquisition; no schedule file
is read, written or repaired.

The current-only 36-byte discovery guard remains in the existing budget NVS
namespace. It contains two operational counters and an active-run marker;
the counters' day identifies the run's credit lifetime across UTC midnight.
The route, season, traversal step and source/access fingerprint remain in RAM.
A reboot restarts traversal from the first source with the remaining durable
credits, forfeiting unused reserved credits rather than replenishing the run's
sixteen-request allowance. Losing RAM evidence does not reset this guard.
Changed source inputs retire the old run before another source can send,
forfeiting its remaining credits without granting another same-day run. At most
one new run starts per UTC day and at most two seasons are considered; ordinary
due/live jobs, setup and provider limits retain priority.

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


### Single application-config owner — 2026-10-18

General settings, competitions, screen policy and exact names-version reference
are one coherent AppConfig. The settings service writes only one current-format
NVS envelope. There are no independent section setters or
separate general/competition/policy persistent writes. Apply validates the full
combination and expected revision, persists/verifies it, then publishes matching
RAM fields and metadata. Its last-apply ID correlates a later client readback.
Any uncertain NVS mutation leaves the old RAM config coherent and changes the
storage origin to `outcome_unknown`, blocking writes until reboot/recovery.
Changed names are staged and validated before that one NVS commit; its exact
reference activates the coherent file/configuration combination. Settings-only
Apply never opens or rewrites names. Both slots survive an uncertain NVS write.
