# Data flow

How the firmware assembles football data today: which requests it sends,
how it filters, maps, combines, caches and publishes the responses, and the
bounds every request must fit. What is stored and shown is described in
[DATA_MODEL.md](DATA_MODEL.md); provider facts, limits and endpoints in
[DATA_PROVIDERS.md](DATA_PROVIDERS.md).

Implementation: `components/football/` (`football_service.cpp`,
`provider_mappers.cpp`, `provider_client`, `request_budget`, `budget_ledger`).
Four adapters and per-competition routing are implemented in the 0.1.0-dev
candidate. The C6 build compiles. Firmware requests and keyed-provider
coverage have not been accepted on the device.

## 1. Pipeline

1. **Settings** supply the competition list (up to three entries), each with
   a primary source and an optional fallback source of another provider, the
   fixture pairs and the per-provider limits
   ([CONFIGURATION.md](CONFIGURATION.md)).
2. **Worker round:** one provider task fetches every active competition in
   list order (section 2).
3. **Routed fetch** per competition: the primary source, then the fallback
   source when it is active and needed (section 6).
4. **Provider client:** one HTTPS request at a time for all providers,
   spaced and capped per provider (sections 2 and 8).
5. **Streaming filter:** the JSON body is parsed element by element; each
   match is kept or discarded while streaming (section 4).
6. **Mapper:** one provider element becomes one canonical record
   (section 5) in the route scratch snapshot.
7. **Compose:** a matched fallback fills missing fields of the primary
   (section 6).
8. **Candidate snapshot:** all competitions of the round; a failed
   competition carries its earlier data (section 7).
9. **Reconcile and publish:** score-change times, finished times and event
   removals are derived against the previous snapshot; the candidate becomes
   the published snapshot (section 7).
10. **Readers** (display presenter, REST status) copy what they need under a
    short mutex (section 7).

## 2. Rounds and polling

One worker task runs rounds. After a round, one delay until the next round is
chosen:

| Situation | Delay after a completed round |
|---|---|
| Outside relevant windows | 6 hours, shortened to the next known window start |
| Before kickoff / between fixtures of the same round | 5 minutes |
| Active fixture | 60 seconds |
| Confirmed post-match window | 5 minutes |
| Ordinary error | 60 seconds |
| HTTP 429 / daily cap reached / timeout | 5 minutes |

Match windows span the first kickoff through the last finish of each
relevant round, including gaps between fixtures; round number is preferred,
then the round text, else the UTC date. An unknown end is capped at kickoff
plus 240 minutes. The before/after offsets default to 30 minutes.

The REST status reports when the next round is due and why
(`next_request_ms`, `next_request_reason`; [API.md](API.md)).

### Request limits per provider

Each provider has two settings (defaults in the order OpenLigaDB /
API-Football / ESPN / football-data.org):

- **Minimum spacing** (5 / 6 / 30 / 6 seconds, 1–3600): measured between
  request starts and shared by all requests of that provider, including
  selections and multiple competitions. A multi-request round therefore
  takes time beyond the delay above.
- **Daily cap** (0 / 100 / 0 / 0 requests, 0–65535; 0 = no cap): every
  attempt, including selections, counts before TLS starts. A durable 12-byte
  NVS record per provider reserves up to eight requests at once; a restart
  loses unused reserved requests and cannot replenish the day, and moving the
  clock backward cannot reopen a spent day. The day changes at 00:00 UTC. A
  storage failure refuses that provider's request; a damaged record blocks
  only that provider for the current day. A provider with cap 0 has no
  record and causes no storage write. The settings reset erases the records.

Live polling stretches the one round delay according to the observed
requests per round and the remaining requests of every capped provider; the
slowest provider sets the interval. These are local policy values, not
provider guarantees, and they cannot account for other devices sharing an
account or IP address.

## 3. Requests per provider today

Sizes and further endpoints: [DATA_PROVIDERS.md](DATA_PROVIDERS.md). Every
identifier is percent-encoded (RFC 3986) when it is placed into a URL.

### Season

An empty season means the provider's current season. OpenLigaDB and
API-Football need it in their URLs, so the device resolves it with one
request (OpenLigaDB: `/getmatchdata/{league}`; API-Football:
`leagues?id=…&current=true`) and caches it per competition and source for six
hours. ESPN and football-data.org select their current season themselves; a
configured season is forwarded. No rule guesses a season from the local
calendar, except the OpenLigaDB competition list in the setup wizard: it uses
the typed season, otherwise the season derived from the device clock (from
July the season named after the current year, before that the previous one).

### OpenLigaDB

| Request | When |
|---|---|
| `/getcurrentgroup/{league}` | every round |
| `/getlastchangedate/{league}/{season}/{round}` | every round (current round), plus each other round with a retained fixture inside its match window |
| `/getmatchdata/{league}/{season}/{round}` | when the change date of that round changed, a baseline is required or its last fetch is one hour old |
| `/getmatchesbyteamid/{team}/{weeks}/{weeks}` | with the round request; filtered to this competition and season |
| `/getmatchdata/{league}/{season}` | whole season, once per competition and UTC day, primary source only, instead of the round and club requests |
| `/getbltable/{league}/{season}` | table competition, with a full refresh |
| `/getavailableleagues/{season}`, `/getavailableteams/{league}/{season}` | selection lists in the setup wizard |

A full refresh of the table competition needs five requests with an explicit
season, six when it first resolves the season; at five-second spacing the
last one starts at least 20 or 25 seconds after the first. A cheap round
needs two requests plus one per active other round.

The whole season is the only OpenLigaDB source of other clubs' catch-up
fixtures outside the current round (there is no date-range request). It is
mapped one fixture at a time in the 1 MiB body class. If it fails (for
example at the 30-second deadline), the round continues with the round and
club requests, and the stream is tried again in the next round.

### API-Football

| Request | When |
|---|---|
| `fixtures?league&season&from&to` (±7 days) | every round |
| `standings?league&season` | every round, table competition |
| `fixtures/events?fixture=…` | every round, once per own match in the ±7-day list, whatever its phase |
| `leagues?id&current=true` | season resolution (above) |
| `leagues?country=…` (default Germany; World for international competitions), `teams?league&season` | selection lists |

### ESPN

| Request | When |
|---|---|
| `{slug}/scoreboard?dates=YYYYMMDD` (current UTC day) | every round |
| `{slug}/teams/{team}/schedule?fixture=true` | every round (upcoming own matches) |
| `{slug}/teams/{team}/schedule` | every round (own results) |
| `site.web…/{slug}/standings` | every round, table competition |
| `leagues/dropdown`, `{slug}/teams` | selection lists |

The device does not request the ESPN match summary.

### football-data.org

| Request | When |
|---|---|
| `competitions/{code}/matches?dateFrom&dateTo[&season]` (±7 days) | every round |
| `competitions/{code}/standings[?season]` | every round, table competition |
| `competitions`, `competitions/{code}/teams` | selection lists |

The **table competition** is the first active competition of the list; no
other competition requests a table.

## 4. Filtering while streaming

- Own matches are kept within seven days on either side of current UTC.
- OpenLigaDB additionally keeps the provider's current round.
- Other clubs' matches outside the current round (catch-up fixtures, the
  neighbouring round) are kept within 36 hours, at every provider.
- Duplicate fixture ids within a competition are ignored.
- Discarded bytes still count toward the transport limits (section 8).

All competitions share the 32 match places of one snapshot. When the list is
full, a further match takes the place of the other club's match whose
kickoff is farthest from the time of the fetch (a match without kickoff
counts as farthest), provided it kicks off nearer than that match or
belongs to the tracked club; the removed match's events leave the pool. The
tracked club's matches are never removed; a match that does not fit is left
out. Either way the snapshot is marked (`matches_truncated`). Without a valid
time nothing is removed. Removal detection of events is disabled for
truncated lists, so eviction cannot pretend that a goal was withdrawn. A
table that does not fit rejects the publication of the round.

Events are kept only for the own club's matches; other matches carry scores
only. Each match references at most 12 events in the shared 32-event pool;
after 12 the oldest entry of that match is reused and `events_truncated` is
set.

## 5. Mapping rules

Definitions of every field: [DATA_MODEL.md](DATA_MODEL.md). Unknown or
missing values stay unavailable; nothing is fabricated.

### Teams

Short name: OpenLigaDB `shortName`, ESPN `shortDisplayName`, football-data
`shortName`; API-Football has none. An empty short name is filled with the
full name.

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
  character after 32 bytes); in/out stay empty because ESPN does not say who
  comes on.
- **API-Football:** `fixtures/events`: goal types, cards, substitution
  (player out, player in from `assist`), VAR, minute and added time.
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

## 6. Combining providers

Each competition may have one fallback source of another provider. Up to
eight explicit fixture pairs bind a primary and a fallback match id within a
configured competition. Enabling a pair certifies that identity and
home/away order were verified; names or search suggestions never certify it,
and there is no automatic cross-source matching.

The fallback is fetched when it is active and the primary failed, a fixture
pair of this competition is enabled, or the table competition's primary
returned no table.

- **Primary succeeded, pair matched:** confirmed primary fields win. The
  fallback fills missing or estimated phase, minute, kickoff and all five
  scores (swapped pairs swap every score), plus richer events. Scored goals
  of the fallback match are dropped when the primary match has a scored goal;
  its missed penalties are dropped when the primary has a missed penalty of
  its own. Fields keep the supplying provider.
- **Primary without a table:** the fallback table is used, tagged with the
  fallback provider.
- **Primary failed:** the whole fallback route is used with its own ids and
  the notice "fallback active".
- **Fallback failed while the primary succeeded:** the primary data publish;
  the fallback error is kept as a routing notice.

Fallback sources use neither the OpenLigaDB cache, the round marks nor the
season stream: they request round, club and table in every round.

## 7. Caches, reconciliation and publication

### Caches

The OpenLigaDB RAM cache reuses primary data only when the round and change
date match, no new baseline is required and the full fetch is less than one
hour old. Two other rounds with a retained fixture inside their match window
are remembered per competition with their own change date; a further one is
fetched in every round. Fixtures of other rounds outside their match window
keep their last values until the next season stream. The season stream and
resolved seasons are cached for six hours.

Caches are not persisted across boots. A save that changes provider settings
(competitions, sources, fixture pairs, keys, ESPN opt-in, demo mode) clears
them and the published snapshot; other saves keep both. A network change or
a clock step requires a new baseline: the first valid time, a time-zone
change or a wall-clock jump of more than two seconds. The regular SNTP
resync neither starts a refresh nor a new baseline.

### Reconciliation

Against the previous snapshot (same competition, provider and match id):

- `score_changed_ms` is set when the principal score changed; switching
  provenance alone does not count.
- `finished_at` is set when a match is first seen finished: now, or the
  expected end when it was never seen running (e.g. after a restart).
- Events missing from a complete later list become one-publication removal
  records.
- The first fetch, a reconnect, a provider settings change and a clock step
  establish a baseline; they do not replay historical goals.

### Publication

Every request carries the provider settings generation and the network
epoch; changed provider settings, network loss/reconnect or an expired
deadline invalidate work before publication.

Publication waits until every active competition has been fetched or has
failed. A failed competition keeps its previously published matches, events
(without removal markers, estimated again) and, for the table competition,
the table, with the time of its last successful fetch; the others publish
their new data in the same snapshot. Such a snapshot is marked stale and
carries the error and cause of the first failed competition and the time of
its next attempt (`retry_ms`). A failed competition is retried after its own
delay (one minute; five minutes after a rate limit or a used cap); until
then later rounds carry its earlier data without a request. A refresh
request, a settings save, a network change or a clock step retries it at
once.

When every active competition fails, or settings or the network change
during the round, nothing is published; the previous snapshot stays with
stale/error state and `retry_ms`. The snapshot fetch time is the oldest
competition fetch time; it is shown as "as of" on the display and as the
data age in the REST status. An age of 15 minutes also marks the data stale.

### Readers

Two static snapshot slots and one route scratch snapshot cost 81,504 B
(placed in BSS). A reader runs a short callback under the repository mutex
and copies into its own fixed model; it performs no I/O, LVGL work,
allocation or blocking call there and keeps no snapshot reference. The
writer reuses the old slot after publication because every read has already
released it.

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
  not sized for large bodies.
- Bodies: 256 KiB for ordinary requests (including the OpenLigaDB round and
  club requests); 1 MiB for selection lists, OpenLigaDB season resolution
  and the daily OpenLigaDB season stream.
- Headers 8 KiB, additional wire/framing allowance 32 KiB, JSON nesting 16,
  live parser allocations 24 KiB including allocation headers (libc overhead
  additional). Elements are mapped and released one at a time; discarded
  fields still count toward byte and depth validation.
- A malformed, excessive or incomplete response fails its competition (see
  publication); a response is never published partially.
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
