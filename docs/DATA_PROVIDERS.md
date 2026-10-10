# Data providers

Four adapters and per-competition routing are implemented in the 0.1.0-dev
candidate. Public endpoint research was refreshed on 2026-10-10; the C6 build
compiles. Firmware requests and keyed-provider coverage have not been accepted
on the device. Provider plans, quotas and supported competitions can change.

## Sources and capabilities

| Provider | Authentication | Adapter data | Limits / acceptance |
|---|---|---|---|
| OpenLigaDB | none | competitions, teams, round/season fixtures, table, goals | community data; phase/minute estimated when absent |
| API-Football | x-apisports-key | competitions, teams, fixtures, standings, own-club goals/cards/substitutions/VAR | configured daily budget; actual competition/tier coverage needs a key |
| ESPN site API | explicit opt-in, no key | competitions, teams, current-day scores, own schedules, table, supplied event details | unofficial interface, no stability contract |
| football-data.org v4 | X-Auth-Token | competitions, teams, fixtures, standings, supplied score phases | actual account permissions/coverage need a key; no fabricated minute/events |

Capability flags describe mapper potential rather than proof that any particular
fixture includes a field. Injury is a canonical event type and an ESPN detail
may carry it; no general injury-feed coverage is promised. Club crests are
uploaded user assets; provider SVG/large PNG files are not decoded by firmware.
The interface also reports each provider's configured minimum request interval.
Its crest-import flag is false while automatic browser import remains deferred.

The 2026-10-08 coverage research found OpenLigaDB German leagues down to
Regionalliga Nord, Nordost and Bayern, plus DFB-Pokal and UEFA data; it did not
confirm current RL West/Südwest coverage. Use the live competition list rather
than that historical snapshot as an availability guarantee. Optional paid/free
plan prices and exact account quotas are deliberately not hard-coded here.

## OpenLigaDB

Base: [OpenLigaDB API](https://api.openligadb.de/).

| Path | Use |
|---|---|
| /getavailableleagues | searchable/paged competition and provider-season selection |
| /getavailableteams/{league}/{season} | club selection |
| /getmatchdata/{league} | current provider season when configuration leaves it empty |
| /getcurrentgroup/{league} | current round identity |
| /getlastchangedate/{league}/{season}/{round} | cheap change check |
| /getmatchdata/{league}/{season} | season stream, retaining current-round and horizon-relevant fixtures |
| /getbltable/{league}/{season} | primary league table |

Unchanged round data reuses primary-provenance values. A full refresh still
runs at least every five minutes to discover catch-up fixtures and table
changes. The current season is re-resolved after six hours.

Results use the provider's semantic
[resultTypeKind contract](https://github.com/OpenLigaDB/OpenLigaDB-Samples/discussions/136).
Unknown kinds stay unavailable. Public DFB 2024 responses confirmed that
AfterPenalties contains the combined result; a known AfterExtraTime base is
subtracted for the separate shootout tally. The research also found inconsistent
older regulation fields, so fixture tests must preserve and exercise those
source contradictions rather than silently relabel them.

## API-Football

Base: [API-Sports football documentation](https://www.api-football.com/documentation-v3).

Selection requests use leagues?country=... (default Germany; World for
international competitions) and teams?league=...&season=.... Country is a
bounded selection argument, not a second persistent configuration field.
Enabled routes require an explicit season. Match requests use
fixtures?league=...&season=...&from=...&to=..., then primary-league standings.
Own retained fixtures additionally request fixtures/events?fixture=....

Every attempt, including selections, consumes the local budget before TLS
starts. A durable 24-byte NVS ledger reserves up to eight attempts at once.
Restarting loses unused credits and cannot replenish the day. Moving the clock
backward cannot reopen a spent day. Budget-storage failures refuse requests.
Normal settings resets retain this ledger.

Live polling stretches conservatively according to observed requests per refresh
and remaining credits. This cannot control other devices sharing a provider
account/IP. No keyed API request or paid-tier coverage was tested in this session.

## ESPN

Selection: site.api.espn.com/apis/site/v2/leagues/dropdown and
.../sports/soccer/{slug}/teams. Scores: the same site's scoreboard?dates=YYYYMMDD.
Own fixtures: teams/{id}/schedule, both past results and fixture=true upcoming
schedule, then bounded local horizon filtering. Table:
site.web.api.espn.com/apis/v2/sports/soccer/{slug}/standings.
A configured season is forwarded to supported route/team endpoints.

Public research verified nested standings entries, integral floating-point
statistics, score strings on scoreboards, score objects on schedules and dates
without seconds. Multi-day scoreboard ranges repeatedly returned HTTP 400,
so the adapter uses a single UTC day plus the club schedules. Rich details
use semantic event flags/text; unknown fields stay unavailable. A public 2022
World Cup final response confirmed principal 3:3 and separate shootoutScore 4:2.
No numeric status/result ID is treated as a semantic constant.

Public research is not firmware/device acceptance. Unofficial endpoint changes,
event direction/coverage, empty schedules and season transitions need recorded
fixtures and operational tests before release.

## football-data.org

Base: [v4 documentation](https://docs.football-data.org/general/v4/).

Selection uses competitions and competitions/{id}/teams. Routes use
competitions/{id}/matches with a bounded date range and standings. The optional
configured season is forwarded; an empty one selects the provider's current
season. Competition selection reads the supplied current-season start year.

The [overtime contract](https://docs.football-data.org/general/v4/overtime.html)
distinguishes regularTime, additional extraTime and penalties. Principal goals
use regulation plus extra-time goals; shootout-inclusive fullTime is not ordinary
goals. The [competition filters](https://docs.football-data.org/general/v4/competition.html)
define season selection. Key/account permission errors remain explicit.

## Polling and request bounds

| Situation | Delay after a completed refresh |
|---|---|
| Outside relevant windows | 6 hours, shortened to the next known window start |
| Before kickoff / between same-round fixtures | 5 minutes |
| Active fixture | 60 seconds |
| Confirmed post-match window | 5 minutes |
| Ordinary error | 60 seconds |
| HTTP 429 / exhausted budget / timeout | 5 minutes |

Individual requests additionally respect conservative minimum spacing:
OpenLigaDB/ESPN 30 seconds; API-Football/football-data 6 seconds. A multi-request
refresh therefore takes time beyond the configured delay. These are local
policy values, not provider service-level guarantees.

HTTPS verifies the full IDF CA bundle and requires a valid clock. No redirect
is followed automatically. One heavy operation token serializes TLS/parsing
with uploads, image validation/deletion and OTA. A routine LVGL JPEG redraw can
still overlap TLS inside its fixed pool; that overlap needs device measurement.

The client checks a 30-second monotonic request deadline across asynchronous
DNS, incremental TLS connection/handshake, headers, framing and reads; socket
wait slices are at most one second. One fixed token-protected DNS job runs in
the existing lwIP thread. Numeric IPv4 connection retains the original hostname
for certificate verification and SNI. No other task closes a live TLS handle.
SDK scheduling and cryptographic step duration still require measurement;
this is not a measured hard real-time bound. Generation/epoch checks prevent
obsolete publication.

Ordinary bodies are limited to 256 KiB; season/selection streams to 1 MiB.
Headers are 8 KiB, additional wire/framing allowance 32 KiB, JSON nesting 16,
and live parser allocations 24 KiB including the custom allocation headers
(libc allocator overhead is additional). Elements are mapped and released one
at a time. All discarded fields still count toward byte/depth validation.
Malformed, excessive or incomplete responses keep the last complete snapshot
and report failure; they do not publish a partial replacement.

## Combining providers

Each route may have one alternate provider. Up to eight explicit fixture
pairs bind primary and secondary IDs to a configured competition/season/club.
Enabling a pair certifies that identity and home/away order were verified.
Names/search suggestions do not certify it.

Confirmed primary fields win. A matched fallback fills missing/estimated
status, minute, kickoff and scores, plus richer events. Primary goal events
take precedence over alternate goal events. Swapped home/away order also
swaps every supplied score pair. Fields retain the supplying provider.
A primary-route failure can use the whole alternate route with its own IDs
and a fallback notice; there is no automatic cross-source identity claim.

## Verification still required

Recorded fixtures must cover missing/null/real-zero values, all score phases,
duplicate/corrected/removed events, own goals, substitutions, source conflicts,
capacity overflow, UTF-8 and season rollover. Network checks cover trickled
headers/body, framing, TLS/DNS failure, 429, cancellation, budgets across reset/
clock changes and configuration while a request runs. Real keys and maximum
configured data are required for coverage and resource acceptance. None of
these suites was run for this candidate.
