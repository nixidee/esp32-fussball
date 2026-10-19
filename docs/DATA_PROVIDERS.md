# Data providers

Reference for the football data providers: who supplies what, their limits
and conditions, all endpoints with measured sizes, and the research results.

Related documents:

- [DATA_MODEL.md](DATA_MODEL.md) — what the device stores, which provider
  fills which field, where it is shown.
- [DATA_FLOW.md](DATA_FLOW.md) — which requests the firmware sends today, how
  it maps, retains RAM facts and publishes the data, and its request bounds.
- [API.md](API.md) — the device's own REST API (not a provider).

All checks were made on **2026-10-13** with public requests from a desktop
machine unless stated otherwise. Provider plans, coverage, identifiers and
limits change; recheck before relying on a value.


The [current screen system](UI.md#current-screens-and-automatic-sequence)
requires the same available information for ordinary and own Single screens,
known schedule coverage for a configurable long-break gap, and trustworthy
period/added-time/shootout provenance. These capabilities are not implemented
merely by selecting a new screen. OpenLigaDB's checked Match schema lacks a
live period/minute and a complete missed-attempt list; an estimated clock must
remain visibly estimated. ESPN complete attempts were found in historical
summary shootout arrays, not just scored-goal lists; live completeness and
request frequency still require proof. API-Football/football-data detail and
coverage remain provider-dependent. Never infer a miss from an absent goal or
claim live support from historical replies. The reference/research sections below retain their check dates. Current typed
adapters stream through one grammar; full-season retention uses two passes,
and explicit ESPN seasons use verified season references rather than an ignored
scoreboard query. See [current acquisition](DATA_FLOW.md#3-request-plans) for
implemented requests and [bounds](DATA_FLOW.md#8-request-bounds) for current
transport ceilings. Current public replay and synthetic keyed-provider fixtures
do not establish current keyed live-data coverage. [OpenLigaDB schema](https://api.openligadb.de/swagger/v1/swagger.json),
[football-data live duration](https://docs.football-data.org/general/v4/overtime.html).


## 1. Priority

ESPN and OpenLigaDB are the primary providers and the focus of current
request-optimization work. API-Football and football-data.org stay supported;
the common scheduler enforces the configured daily caps and window limits
for all four providers. Further refinement of the keyed-provider request
plans is deferred for now:

- API-Football free plan: 100 requests per day, which is not enough for live
  polling of more than one match.
- football-data.org free plan: scores and schedules are delayed, no goal
  scorers, cards or substitutions, and only four of the relevant
  competitions.

## 2. Providers

| Provider | Kind | Key | Base URL |
|---|---|---|---|
| OpenLigaDB | community-maintained open data (ODbL 1.0) | none | `https://api.openligadb.de` |
| ESPN site API | unofficial interface of espn.com, no contract; used only after an explicit opt-in in the settings | none | `https://site.api.espn.com`, `https://site.web.api.espn.com`, `https://sports.core.api.espn.com` |
| API-Football (API-Sports) | commercial API, free plan | `x-apisports-key` header | `https://v3.football.api-sports.io` |
| football-data.org v4 | commercial API, free plan | `X-Auth-Token` header | `https://api.football-data.org/v4` |

### Legend

| Mark | Meaning |
|---|---|
| ✓ | verified with a real response on the check date |
| (✓) | available, but incomplete or limited (see note) |
| ✗ | verified as not available |
| € | only in a paid plan (price in the cell) |
| ? | not verified (documentation not reachable or a key is needed) |
| doc | taken from the provider documentation, not from a real response |

### How sizes were measured

Sizes are uncompressed response bodies; the firmware does not send
`Accept-Encoding`, so the device receives uncompressed bodies. Reference
requests: Bundesliga 2026/27, Werder Bremen (OpenLigaDB team 134, ESPN team
137). Sizes of the keyed providers were not measured.

## 3. Limits

| | OpenLigaDB | ESPN | API-Football (free) | football-data.org (free) |
|---|---|---|---|---|
| **Time limit** | 60 per minute per IP (doc) | none published | ? not published where checked; secondary sources: 10 per minute | 10 per minute (doc); 20–120 per minute in paid plans |
| **Daily limit** | none (~86,400 at 60/min) | none published (unknown) | **100 per day** (doc), reset 00:00 UTC | none for registered clients (~14,400 at 10/min); without a key 100 per 24 h (doc), **50 measured** |
| Key / account | no | no | yes | yes (without a key only area and competition lists) |
| Other rules | "please fetch sparingly"; poll `getlastchangedate` before full data | unofficial; may change or block without notice | quota counter in response headers | scores and schedules **delayed**; bans clients that poll too often |

Time limits of OpenLigaDB, ESPN and football-data.org count per IP address
or account: other devices or programs behind the same internet connection or
with the same key use the same budget. Figures in brackets ("~N") are
extrapolations, not published quotas.

## 4. Competition coverage

Cell content: the provider's identifier when available. OpenLigaDB
identifiers are community entries; the list contains many test, duplicate
and abandoned entries, and the identifiers below are the ones with real data
on the check date. They can change every season.

### 4.1 German club competitions

| Competition | OpenLigaDB | ESPN | API-Football | football-data.org |
|---|---|---|---|---|
| Bundesliga | ✓ `bl1` | ✓ `ger.1` | ? | ✓ `BL1` (free) |
| 2. Bundesliga | ✓ `bl2` | ✓ `ger.2` | ? | € `BL2` (49 €/month) |
| 3. Liga | ✓ `bl3` | ✗ | ? | € `BL3` (99 €/month) |
| Regionalliga Nord (level 4) | (✓) `rln` – no goal minutes, no scorers | ✗ | ? | € `RLN` (199 €/month) |
| Regionalliga Nordost (level 4) | (✓) `rlno` – almost no goal minutes or scorers | ✗ | ? | € `RNO` (199 €/month) |
| Regionalliga Bayern, West, Südwest (level 4) | ✗ (`regio-bayern` empty) | ✗ | ? | € `RBY`, `RLW`, `RSW` (199 €/month) |
| Oberliga (level 5) | (✓) only NOFV Nord/Süd listed (`nofvon`, `nofvos`), data not checked | ✗ | ? | ✗ |
| DFB-Pokal | ✓ `dfb` | ✓ `ger.dfb_pokal` | ? | € `DFB` (49 €/month) |
| Landespokal | ✗ | ✗ | ? | ✗ |
| DFL Supercup | ✓ `BLSupercup` | ✓ `ger.super_cup` | ? | € `GSC` (199 €/month) |
| Relegation Bundesliga / 2. Bundesliga | (✓) `rel` (season 2025 = May 2026: the 2 matches; extra-time result missing, see 6.3) | ✓ `ger.playoff.relegation` (May 2026, with goals and cards) | ? | € `BLREL` (199 €/month) |
| Relegation 2. Bundesliga / 3. Liga | ✗ no entry found | ✓ `ger.2.promotion.relegation` (May 2026, with goals and cards) | ? | € (in `BLREL`, not checked) |

### 4.2 International club competitions

| Competition | OpenLigaDB | ESPN | API-Football | football-data.org |
|---|---|---|---|---|
| Champions League | ✓ `ucl` (`ucl2026` is an incomplete duplicate) | ✓ `uefa.champions` | ? | ✓ `CL` (free) |
| Europa League | ✓ `uel2026` (`uel` holds the 2025 knockout phase) | ✓ `uefa.europa` | ? | € `EL` (49 €/month) |
| Conference League | ✓ `uecl2026` (first match 2026-10-15) | ✓ `uefa.europa.conf` | ? | € `UCL` (199 €/month) |
| UEFA Super Cup | ✗ | ✓ `uefa.super_cup` | ? | € `ESC` (199 €/month) |
| Qualifying rounds | not checked | `uefa.champions_qual`, `uefa.europa_qual`, `uefa.europa.conf_qual` listed, data not checked | ? | € `CLQ` (49 €/month), `ELQ` (99 €/month), `COLQ` (199 €/month) |

### 4.3 National team (Germany)

| Competition | OpenLigaDB | ESPN | API-Football | football-data.org |
|---|---|---|---|---|
| World Cup | ✓ `wm26` (2026, complete; `wm2026_xlife`, `WC2026`, `dfb-wm26` are duplicates or empty) | ✓ `fifa.world` | ? | ✓ `WC` (free) |
| World Cup qualifying (UEFA) | not checked | ✓ `fifa.worldq.uefa` | ? | € `QUFA` (199 €/month) |
| European Championship | not checked (next tournament 2028) | ✓ `uefa.euro` (latest season 2024) | ? | ✓ `EC` (free, latest season 2024) |
| EURO qualifying | not checked | ✓ `uefa.euroq` | ? | € `ECQ` (199 €/month) |
| Nations League | ✓ `nla` (League A 2026; `unl` empty) | ✓ `uefa.nations` | ? | € `UNL` (199 €/month) |
| Friendlies | (✓) `DFBN` (6 matches) and `DFBNAT2526` (season 2025, 2 matches), user-maintained | ✓ `fifa.friendly` (all national teams) | ? | € `IFL` (199 €/month) |
| Women's EURO | (✓) `Fef` (2025) | not checked | ? | ? |

### 4.4 Result for the target competitions (ESPN + OpenLigaDB)

| Target | Covered by | Restriction |
|---|---|---|
| Bundesliga, 2. Bundesliga | both | – |
| 3. Liga | OpenLigaDB only | no minute, no cards, no substitutions; 81 % of goals with minute, 65 % with scorer (6.2) |
| Level 4 (Regionalliga) | OpenLigaDB only, Nord and Nordost only | no goal minutes or scorers; the other three Regionalligen are not covered |
| DFB-Pokal, DFL Supercup | both | – |
| Relegation | ESPN for both relegations; OpenLigaDB only Bundesliga / 2. Bundesliga | OpenLigaDB extra-time result missing (6.3) |
| Champions, Europa, Conference League | both | UEFA Super Cup and qualifying only ESPN |
| World Cup / EURO qualifying | ESPN | OpenLigaDB not checked |
| Friendlies of the national team | ESPN (`fifa.friendly`, all nations); OpenLigaDB user entries | OpenLigaDB entries are incomplete and change per season |

## 5. Live score and event coverage

### 5.1 Per provider

| Data | OpenLigaDB | ESPN scoreboard | ESPN match summary | API-Football | football-data.org free |
|---|---|---|---|---|---|
| Live score | ✓ (entered by the community, delay unknown) | ✓ | ✓ | ? doc | (✓) **delayed**, amount not published |
| Live status / phase | (✓) only a "finished" flag | ✓ | ✓ | ? doc (status codes) | (✓) delayed |
| Minute | ✗ | ✓ `displayClock` | ✓ | ? doc (`elapsed`) | ? doc (`minute`, `injuryTime`) |
| Round number / label | ✓ `group` | ✗ | ✗ | (✓) doc: text label only (`league.round`) | ✓ doc (`matchday`, `stage`) |
| Half-time score | ✓ | ✗ | ✓ | ? doc | ✓ doc |
| Score after regulation / extra time | ✓ (extra time incomplete, 6.3) | ✗ (only principal score and status `STATUS_FINAL_AET`) | ✓ | ? doc | ✓ doc |
| Penalty shootout tally | ✓ (combined with earlier goals, 6.3) | ✓ `shootoutScore` | ✓ | ? doc | ✓ doc |
| Goals (scorer, minute) | ✓ (quality differs, 5.2) | ✓ | ✓ | ? doc | € (29 €/month) |
| Own goal / penalty flag | ✓ | ✓ | ✓ | ? doc | € |
| Missed penalty | ✗ | ✗ in the samples | ? | ? doc | € |
| Yellow card | ✗ | ✓ | ✓ | ? doc | € |
| Second yellow / red card | ✗ | ✓ red only | ✓ | ? doc | € |
| Substitutions | ✗ | ✗ | ✓ | ? doc | € |
| Whistles (kick-off, half-time, 2nd half, full time) | ✗ | ✗ only as status | ✓ (with wall-clock time) | ✗ only as status changes (doc) | ✗ |
| VAR | ✗ | ✗ | ✗ in the sample | ? doc | ✗ |
| Delays, injuries | ✗ | ✗ | ✓ start/end of a delay | ? | ✗ |
| Size per request | 25 B (change check), 1.6 KB (one match), 18 KB (round) | 2–127 KB (one day, one competition) | 179–398 KB (one finished match) | ? | ? |

The ESPN match summary exceeds the device's 256 KiB body limit for ordinary
requests; the device reads it only for the own match's substitutions, with
its own 512 KiB limit (bounds in [DATA_FLOW.md](DATA_FLOW.md)).

### 5.2 Quality per competition (verified samples)

**OpenLigaDB, finished matches of the 2026/27 season:**

| Competition | Finished | Goal lists complete | Goals with minute | Goals with scorer |
|---|---|---|---|---|
| Bundesliga | 45 | 45/45 | 172/172 | 172/172 |
| 2. Bundesliga | 63 | 63/63 | 203/204 | 203/204 |
| 3. Liga | 80 | 80/80 | 221/272 | 178/272 |
| DFB-Pokal | 32 | 32/32 | 148/177 | 177/177 |
| Champions League | 18 | 18/18 | 69/69 | 69/69 |
| Europa League | 18 | 18/18 | 47/47 | 47/47 |
| Regionalliga Nord | 106 | 106/106 | **0/409** | **0/409** |
| Regionalliga Nordost | 108 | 108/108 | 10/342 | 11/342 |
| Nations League A | 32 | 32/32 | 97/97 | 96/97 |
| World Cup 2026 | 104 | 103/104 | 307/333 | 332/333 |

How fast scores are entered during a live match was not measured; it depends
on the community and differs by competition.

**ESPN scoreboard, event types seen** (finished matches on the sample day):

| Competition | Sample | Event types |
|---|---|---|
| Bundesliga | 2026-09-19, 5 matches | goal (incl. header, volley), penalty, yellow |
| DFB-Pokal | 2026-08-22 and 23, 22 matches | goal, own goal, penalty (incl. shootout), yellow, red |
| DFL Supercup | 2026, 1 match | goal, yellow, red |
| Relegation Bundesliga / 2. Bundesliga | May 2026, 2 matches | goals and cards (2nd leg after extra time) |
| UEFA Super Cup | 2026, 1 match | goal, yellow |
| Champions / Europa / Conference League | no finished match on the sampled days | not sampled |
| Nations League | 10 matches | goal (header, volley), own goal, penalty, yellow, red |
| World Cup | final 2026 | goal, yellow, red |
| World Cup qualifying | 4 matches | goal (header, volley), penalty, yellow, red |
| European Championship | final 2024 | goal, yellow |
| EURO qualifying | 3 matches | goal, penalty, yellow, red |
| Friendlies | 10 matches | goal (header, free kick), penalty, yellow, red |

Across these ESPN samples the scoreboard contained goals, penalties, own
goals and yellow/red cards, with the observed subset listed per competition
above; none contained substitutions. This supports a shared event mapper
for the sampled responses. It does not establish equal completeness,
update timing or parser-memory needs across competitions, and the unsampled
UEFA rows remain unverified by this table. OpenLigaDB's observed differences
are recorded in its quality table above.

## 6. OpenLigaDB

### 6.1 General

- Open community database, no registration, no key, no quota. Data licence
  ODbL 1.0. Description: `https://api.openligadb.de/swagger/v1/swagger.json`.
- Published limit: **60 requests per minute per IP**. The provider asks to
  fetch sparingly and to poll `getlastchangedate` before fetching full data.
- Data is entered by volunteers; completeness differs a lot by competition
  (5.2). No minute, no live status, no cards, no substitutions, no whistles.
- Seasons are named by the starting year (2026 = 2026/27).
- Some league shortcuts contain spaces or umlauts and must be URL-encoded.
- The league list also contains other sports (ice hockey) and test entries.

### 6.2 Endpoints

All `GET` without headers. Example values: `bl1`, season `2026`, round `7`,
team `134` (Werder Bremen), league id from the match data.

| Endpoint | Returns | Example | Size |
|---|---|---|---|
| `/getavailableleagues` | all leagues of all seasons | `/getavailableleagues` | 123,425 B |
| `/getavailableleagues/{season}` | leagues of one season | `/getavailableleagues/2025` | 7,874 B |
| `/getavailablesports` | sports | `/getavailablesports` | 504 B |
| `/getavailablegroups/{league}/{season}` | all rounds of the season | `/getavailablegroups/bl1/2026` | 2,125 B |
| `/getcurrentgroup/{league}` | current round | `/getcurrentgroup/bl1` | 60 B |
| `/getavailableteams/{league}/{season}` | teams with short name and crest URL | `/getavailableteams/bl1/2026` | 3,033 B |
| `/getmatchdata/{matchId}` | one match | `/getmatchdata/77186` | 1,583 B |
| `/getmatchdata/{league}/{season}/{round}` | one round | `/getmatchdata/bl1/2026/7` | 17,947 B |
| `/getmatchdata/{league}/{season}` | whole season | `/getmatchdata/bl1/2026` | 299,650 B (bl3: 426,737 B; about twice as large in May) |
| `/getmatchdata/{league}/{season}/{teamFilter}` | season matches of a team (name filter) | `/getmatchdata/bl1/2026/Werder` | not measured |
| `/getmatchdata/{teamId1}/{teamId2}` | all matches between two teams | `/getmatchdata/134/40` | not measured |
| `/getmatchdata/{league}` | current round, no season; works, but is **not** in the API description | `/getmatchdata/bl1` | 17,947 B |
| `/getlastchangedate/{league}/{season}/{round}` | last change of a round | `/getlastchangedate/bl1/2026/7` | 25 B |
| `/getnextmatchbyleagueteam/{leagueId}/{teamId}` | next match of a team | `/getnextmatchbyleagueteam/4741/134` | 829 B |
| `/getlastmatchbyleagueteam/{leagueId}/{teamId}` | last match of a team | `/getlastmatchbyleagueteam/4741/134` | 2,093 B |
| `/getnextmatchbyleagueshortcut/{league}` | next match of the league | `/getnextmatchbyleagueshortcut/bl1` | not measured |
| `/getlastmatchbyleagueshortcut/{league}` | last match of the league | `/getlastmatchbyleagueshortcut/bl1` | not measured |
| `/getmatchesbyteamid/{teamId}/{weeksPast}/{weeksFuture}` | a team's matches in all competitions | `/getmatchesbyteamid/134/1/1` | 2,925 B |
| `/getmatchesbyteam/{teamFilter}/{weeksPast}/{weeksFuture}` | same, by name filter | `/getmatchesbyteam/Werder/1/1` | not measured |
| `/getbltable/{league}/{season}` | league table | `/getbltable/bl1/2026` | 4,382 B |
| `/getgrouptable/{league}/{season}` | group tables (tournaments) | `/getgrouptable/wm26/2026` | not measured |
| `/getgoalgetters/{league}/{season}` | scorer list | `/getgoalgetters/bl1/2026` | 7,346 B |
| `/getresultinfos/{leagueId}` | result types used by a league | `/getresultinfos/4741` | not measured |
| `/getresulttypes` | result kinds (half-time, end result …) | `/getresulttypes` | not measured |

Further measured sizes: `rel` season 2025 (whole season, 2 matches) 3,321 B;
`DFBNAT2526` season 2025 (2 matches) 4,198 B.

The league id (e.g. 4741) is not the shortcut; it is the `leagueId` of the
match data.

Match fields: `matchID`, `matchDateTimeUTC`, `leagueId`, `leagueShortcut`,
`leagueSeason`, `group` (`groupName`, `groupOrderID`), `team1`/`team2`
(`teamId`, `teamName`, `shortName`, `teamIconUrl`), `matchIsFinished`,
`lastUpdateDateTime` (without a time zone in some responses), `matchResults`
(one entry per result kind), `goals` (`goalID`, `matchMinute`,
`goalGetterName`, `scoreTeam1/2`, `isPenalty`, `isOwnGoal`, `isOvertime`),
`location`, `numberOfViewers`.

### 6.3 Result kinds and known data errors

- Results carry a semantic
  [`resultTypeKind`](https://github.com/OpenLigaDB/OpenLigaDB-Samples/discussions/136)
  (half-time, after 90 minutes, after extra time, after penalties). Numeric
  result ids differ per league and are not a contract.
- Public DFB-Pokal 2024 responses: the result after penalties is the combined
  total (earlier goals plus the shootout).
- Older seasons have inconsistent regulation fields.
- **Relegation of season 2025 (May 2026), 2nd leg** (Paderborn – Wolfsburg, 2:1 after extra time
  at ESPN): OpenLigaDB holds only "after 90 minutes 2:1"; the result after
  extra time is missing, and the 100th-minute goal is marked
  `isOvertime: false`. Extra-time results of OpenLigaDB are therefore not
  reliable. Goal minutes differ by one minute from ESPN (39' vs 38').

## 7. ESPN site API

### 7.1 General

- Unofficial interface used by espn.com; no key, no published limit, no
  stability contract. It can change or block at any time.
- `abbreviation` is not reliable (e.g. "MUN" for Bayern München);
  `shortDisplayName` is usable ("Leverkusen", "Hamburg"), but partly English
  ("Cologne").
- League matchdays have no number anywhere (checked 2026-10-14 for
  `ger.1`: site scoreboard, scoreboard header, team schedule, core event and
  competition; the core `weeks` resource is empty for `ger.1` and `eng.1`;
  `week=` on the scoreboard returns no match). The season calendar lists the
  match dates only (7.4).
- Cups and UEFA competitions carry the round as text: each match has
  `season.slug` (e.g. `quarterfinals`, `league-phase`), and the scoreboard
  calendar lists the rounds with label and date range (7.4).
- A substitution detail names both players without an explicit in/out
  field; who comes on follows only from the participant order and the text
  (7.3).
- Extra time is visible only as the status (`STATUS_FINAL_AET`); there are no
  separate regulation or extra-time scores on the scoreboard. The shootout
  tally is separate (`shootoutScore`; 2022 World Cup final: 3:3, shootout
  4:2).

### 7.2 Scoreboard date forms

| `dates=` | Result | Example (ger.1) |
|---|---|---|
| none | the next day with a match (`day.date`), not necessarily today | 14,410 B; on 2026-10-14 it returned 2026-10-16 |
| `YYYYMMDD` | one day | 2–127 KB (67,169 B for 6 Bundesliga matches, 126,690 B for 11 cup matches) |
| `YYYYMM` | one month | 202610: 34 matches, 346,375 B |
| `YYYY` | the first 100 matches of the calendar year (`limit=` raises it) | 2026: 1,047,452 B; with `limit=1000` 297 matches, 2,947,585 B |
| `YYYYMMDD-YYYYMMDD` | HTTP 400 | day ranges do not work (also on `site.web.api`) |

`limit=` works on every form (`dates=202610&limit=5`: 53,884 B). The site
scoreboard costs about 10 KB per match in every form.

### 7.3 Endpoints

All `GET` without headers. `{slug}` from section 4, e.g. `ger.1`.

| Endpoint | Returns | Example | Size |
|---|---|---|---|
| `site.api…/apis/site/v2/leagues/dropdown?sport=soccer&limit=1000` | all competitions with slugs | `https://site.api.espn.com/apis/site/v2/leagues/dropdown?sport=soccer&limit=1000` | 577,429 B |
| `site.api…/apis/site/v2/sports/soccer/{slug}/scoreboard[?dates=…]` | matches with score, status, minute, goals and cards (`details`); league calendar | `…/soccer/ger.1/scoreboard?dates=20261017` | see 7.2 |
| `site.api…/v2/sports/soccer/{slug}/summary?event={id}` | one match: key events (incl. substitutions and whistles), lineups, commentary, statistics, odds, news | `…/soccer/ger.1/summary?event=401884788` | 179,575–398,053 B |
| `site.api…/v2/sports/soccer/{slug}/teams` | teams with short names and logos | `…/soccer/ger.1/teams` | 22,041 B |
| `site.api…/v2/sports/soccer/{slug}/teams/{id}` | one team | `…/soccer/ger.1/teams/137` | 20,250 B |
| `site.api…/v2/sports/soccer/{slug}/teams/{id}/schedule` | the team's results | `…/teams/137/schedule` | 46,005 B |
| `site.api…/v2/sports/soccer/{slug}/teams/{id}/schedule?fixture=true` | the team's upcoming matches | `…/teams/137/schedule?fixture=true` | 237,915 B (29 matches) |
| `site.api…/v2/sports/soccer/{slug}/teams/{id}/roster` | squad | `…/teams/137/roster` | 220,499 B |
| `site.api…/v2/sports/soccer/{slug}/news` | news | `…/soccer/ger.1/news` | 20,996 B |
| `site.web.api…/apis/v2/sports/soccer/{slug}/standings` | table (`?season=` optional) | `https://site.web.api.espn.com/apis/v2/sports/soccer/ger.1/standings` | 64,095 B |
| `sports.core.api…/v2/sports/soccer/leagues/{slug}/events?dates=A-B` | list of match links in a date range | `https://sports.core.api.espn.com/v2/sports/soccer/leagues/ger.1/events?dates=20261001-20261031&limit=100` | 3,771 B (`$ref` link list) |
| `sports.core.api…/…/events/{id}/competitions/{id}/status` | status, clock and added time of one match | `…/events/401884788/competitions/401884788/status` | 343 B |

| `sports.core.api…/v2/sports/soccer/leagues/{slug}/calendar/ondays` | all match dates of the current season (`eventDate.dates[]`, start of each provider day, e.g. `2026-10-16T07:00Z`); also for cups (checked 2026-10-14: `ger.dfb_pokal` 8 dates, 1,347 B; `uefa.champions` 16, 1,525 B; `fifa.friendly` 59, 1,793 B) | `…/leagues/ger.1/calendar/ondays` | 1,710 B (56 dates) |
| `sports.core.api…/…/leagues/{slug}/seasons/{year}/types/1/events?limit=400` | links to all matches of the season | `…/leagues/ger.1/seasons/2026/types/1/events?limit=400` | 33,420 B (306 links) |
| `sports.core.api…/…/leagues/{slug}/events/{id}` | one match (date, teams as links, status as link) | `…/leagues/ger.1/events/401884492` | 9,648 B |
| `site.web.api…/apis/v2/scoreboard/header?sport=soccer&league={slug}&dates=…` | scoreboard variant | `…&league=ger.1&dates=202610` | 412,961 B (34 matches; larger than the site scoreboard, day ranges silently ignored) |
| `cdn.espn.com/core/soccer/fixtures?xhr=1&league={slug}` | web page data: matches of the next three match days | `…&league=ger.1` | 162,523 B |

`site.api…/v2/sports/soccer/{slug}/standings` returns an empty object; tables
come only from `site.web.api`. A competition without table (cups) has no
`entries` list in that response. The core API returns links that need one
further request per match; it was only probed.

Event types: scoreboard `details` contain goal (incl. header, volley, free
kick), penalty scored, own goal, yellow card and red card. The summary
`keyEvents` additionally contain kickoff, half-time, start of the 2nd half,
end of regular time, substitution (both players, with reason) and start/end
of a delay, each with match clock and wall-clock time. In a substitution the
participants are ordered [player in, player out] and the text reads "X
replaces Y" (checked 2026-10-13 on one finished Bundesliga match). In the
398,053 B summary of that match the `keyEvents` start about 298 KB into the
response and take about 14 KB, so a streamed read must allow more than
256 KiB.

Smaller sources for substitutions (checked 2026-10-13, same match): the
core API `…/events/{id}/competitions/{id}/plays?limit=1000` returned
1,556,167 B for 1000 of 1504 plays and contained one substitution; its
`type` filter was ignored (same size). `…/competitions/{id}/details`
returned HTTP 404. The competition resource (8,598 B) contains only links.
No source smaller than the match summary was found.

Response details: nested standings entries with integral floating-point
statistics; score strings on scoreboards and score objects on schedules;
dates without seconds.

### 7.4 Fixtures and rounds (checked 2026-10-14, `ger.1`, season 2026)

- **Match dates of the season:** every scoreboard response carries
  `leagues[0].calendar`; for leagues it is a list of the season's match
  dates (`calendarType` `day`, 56 dates for `ger.1`), the same as the core
  resource `calendar/ondays` (1,710 B). For cups and UEFA competitions it is
  a list of rounds (`calendarType` `list`) with `label` (e.g. "League
  Phase", "Second Round"), `detail` (e.g. "Oct 27-28"), `value`,
  `startDate` and `endDate`.
- **Matches with times:** only the site scoreboard delivers matches with
  teams, kickoff and status in one response: per day (`YYYYMMDD`), per
  month (`YYYYMM`, 34 matches in 346,375 B) or per calendar year
  (`YYYY` with `limit=`). The whole season is about 3 MB.
- **Own club:** `teams/{id}/schedule?fixture=true` lists the club's
  remaining matches of that competition only (29 `ger.1` matches,
  237,915 B, about 8 KB per match); without `fixture=true` the played ones
  (46,005 B).
- **Core API:** day ranges work there (`events?dates=20261016-20261018`:
  9 links in 1,044 B, one matchday), but each match needs one more request
  of about 9.6 KB.
- **Matchday number:** not supplied for leagues (7.1). Postponed matches
  keep their new date, so counting date blocks does not give a reliable
  matchday number.
- **Change check:** no endpoint comparable to OpenLigaDB
  `getlastchangedate` was found.

## 8. API-Football (API-Sports)

Supported, not refined now (section 1).

- Commercial API, account and key needed (`x-apisports-key`). The
  documentation (`https://www.api-football.com/documentation-v3`) is behind a
  browser check and could not be read on the check date; everything marked
  "doc" comes from earlier checks or secondary sources and is **not
  verified**.
- Free plan: **100 requests per day**, reset 00:00 UTC. Quota headers
  `x-ratelimit-requests-limit`, `x-ratelimit-requests-remaining`; per minute
  `x-ratelimit-limit`, `x-ratelimit-remaining`. Per-minute limit of the free
  plan not verified (secondary source: 10).
- Without a key: HTTP 403 with
  `{"errors":{"token":"Missing application key, …"}}` (verified).
- Coverage per league (events, lineups, standings) is reported in the
  `coverage` object of `/leagues`.
- Round: only a text label (`league.round`, e.g. "Regular Season - 7", doc),
  no round number. Teams in match data have no short name.

Endpoints (doc, not verified), all `GET` with the key header:

| Endpoint | Returns | Example |
|---|---|---|
| `/status` | account, plan, quota used today | `/status` |
| `/leagues` | leagues and cups with seasons and coverage | `/leagues?country=Germany`, `/leagues?id=78&current=true` |
| `/teams` | teams (with a 3-letter `code`) | `/teams?league=78&season=2026` |
| `/standings` | tables | `/standings?league=78&season=2026` |
| `/fixtures/rounds` | rounds of a season | `/fixtures/rounds?league=78&season=2026` |
| `/fixtures` | matches by league, team, date range, id, or all live | `/fixtures?league=78&season=2026&from=2026-10-13&to=2026-10-20`, `/fixtures?team=162&next=5`, `/fixtures?live=all`, `/fixtures?id=…` |
| `/fixtures/events` | goals, cards, substitutions, VAR of a match | `/fixtures/events?fixture=…` |
| `/fixtures/lineups`, `/fixtures/statistics`, `/fixtures/players`, `/fixtures/headtohead` | further match data | – |
| `/timezone`, `/countries`, `/leagues/seasons`, `/teams/statistics`, `/teams/seasons`, `/teams/countries`, `/venues`, `/injuries`, `/predictions`, `/coachs`, `/players`, `/players/squads`, `/players/topscorers`, `/transfers`, `/trophies`, `/sidelined`, `/odds` | further data, not needed | – |

The ids in the examples (league 78, team 162) come from secondary sources;
the firmware takes ids from `/leagues` and `/teams` at setup.

## 9. football-data.org v4

Supported, not refined now (section 1).

- Commercial API by one maintainer; key in `X-Auth-Token`. Without a key
  only `/areas` and `/competitions` work (other resources: HTTP 403,
  verified). Anonymous limit: documentation 100 per 24 h, header on the check
  date **50** (`X-Requests-Available: 49` after the first request,
  `X-RequestCounter-Reset: 86400`).
- Free plan (pricing page, verified): 12 competitions, **scores and
  schedules delayed**, league tables, **10 requests per minute**. Relevant
  free competitions: Bundesliga, Champions League, World Cup, European
  Championship.
- Goal scorers, cards and substitutions need at least "Free + Deep Data"
  (29 €/month). Coverage tiers: 49 €/month = 2. Bundesliga, DFB-Pokal, Europa
  League; 99 €/month = 3. Liga; 199 €/month = Regionalliga, Conference
  League, Nations League, super cups, friendlies. The tier per competition is
  the `plan` field of `/competitions` (verified).
- Response headers: `X-Requests-Available` (documented as
  `X-RequestsAvailable`), `X-RequestCounter-Reset`, `X-Authenticated-Client`.
- Clients that poll too often are banned (documentation).
- Date ranges exclude `dateTo` (v4 changelog, checked 2026-10-16); the
  firmware therefore ends every range one day later (live: today to
  tomorrow). Match lists hide goals, bookings, substitutions and lineups
  unless `X-Unfold-Goals`, `X-Unfold-Bookings`, `X-Unfold-Subs`,
  `X-Unfold-Lineups: true` are sent.
- Match statuses: SCHEDULED, TIMED, IN_PLAY, PAUSED, EXTRA_TIME,
  PENALTY_SHOOTOUT, FINISHED, SUSPENDED, POSTPONED, CANCELLED, AWARDED; the
  match page describes nine of them (SUSPENDED and AWARDED are not
  described).
- [Overtime contract](https://docs.football-data.org/general/v4/overtime.html):
  `regularTime`, additional `extraTime` goals and `penalties` are separate;
  `fullTime` includes the shootout goals.
- Round: `matchday` (number) and `stage` (text, e.g. `REGULAR_SEASON`, doc).

Endpoints, all `GET` with the key header:

| Endpoint | Returns | Example |
|---|---|---|
| `/areas`, `/areas/{id}` | countries and continents | `/v4/areas/{areaId}` |
| `/competitions` | competitions with plan and current season (works without key) | `/v4/competitions` (73,804 B) |
| `/competitions/{code}` | one competition | `/v4/competitions/BL1` |
| `/competitions/{code}/standings` | table (`?season=`, `?matchday=`) | `/v4/competitions/BL1/standings` |
| `/competitions/{code}/matches` | matches (`dateFrom`, `dateTo`, `matchday`, `status`, `stage`, `season`) | `/v4/competitions/BL1/matches?dateFrom=2026-10-13&dateTo=2026-10-20` |
| `/competitions/{code}/teams` | teams with `shortName`, `tla`, crest | `/v4/competitions/BL1/teams` |
| `/competitions/{code}/scorers` | top scorers | `/v4/competitions/BL1/scorers` |
| `/matches` | matches of all permitted competitions (`date`, `dateFrom`, `dateTo`, `competitions`, `ids`, `status`) | `/v4/matches?date=TODAY` |
| `/matches/{id}`, `/matches/{id}/head2head` | one match, earlier meetings | `/v4/matches/{matchId}` |
| `/teams/{id}`, `/teams/{id}/matches` | a team; its matches in all competitions | `/v4/teams/{teamId}/matches?dateFrom=2026-10-13&dateTo=2026-10-20` |
| `/persons/{id}`, `/persons/{id}/matches`, `/trends/` | players, form data (paid add-on) | – |

Competition ids (e.g. BL1 = 2002) are verified from `/competitions`.

## 10. Open checks

**OpenLigaDB**
- How quickly scores and goals are entered during live matches, per
  competition (only finished matches were checked).
- Whether `getlastchangedate` changes reliably on every score change.
- Data of `nofvon`/`nofvos`, of EURO and World Cup qualifying entries, and
  whether a 2. Bundesliga / 3. Liga relegation entry appears.
- League identifiers for the next season.

**ESPN**
- Whether ESPN limits or blocks request rates.
- Scoreboard event types on Champions, Europa and Conference League days.
- Whether the scoreboard ever contains substitutions.
- A smaller source for substitutions and whistles than the match summary:
  core API `plays` and `details` checked 2026-10-13 without success (7.3);
  other resources not checked.
- Download time of the ≈ 400 KB match summary on the device.
- Stability of the date forms in 7.2.

**API-Football** (needs a key): per-minute limit, current-season coverage of
the free plan, league ids and `coverage`, events in `fixtures?id=` and
`fixtures?live=all`, event fields, response sizes.

**football-data.org** (needs a key): delay of the free plan, `minute` and
`injuryTime` in the free plan, response sizes, the anonymous daily limit,
`shortName` and `tla` in real responses, the real `stage` values.


## Historical public response recheck — 2026-10-17

At that checkpoint, the captured public replies were replayed through the then
current filtered parser/mappers/store on the host. The measurements below
precede the streaming typed-adapter cutover and its1-MiB full-season bound;
they describe that historical implementation, not current resource limits.
They do not establish device transport, target heap or complete request planning.

| Response | Body bytes | Host parser peak bytes | Result |
|---|---:|---:|---|
| OpenLigaDB Bundesliga current | 7,834 | 6,412 | 9 matches |
| OpenLigaDB Bundesliga whole2026 season | 299,845 | 6,623 before rejection | Exceeds262,144 cap; incomplete, no coverage proof |
| OpenLigaDB cup round | 77,813 | 15,432 | 32 matches |
| ESPN Bundesliga finished scoreboard | 11,822 | 7,303 | Full ordinary-match names |
| ESPN Bundesliga summary | 388,143 | 5,082 | 8 stored ordinary events, own=false |
| ESPN Bundesliga scheduled scoreboard | 71,642 | 6,764 | 6 scheduled matches |
| ESPN cup scoreboard | 126,772 | 11,986 | 11 matches; calendar shape differs, no proof |
| ESPN empty scoreboard | 2,052 | 86 | Successful empty list, no invented fixture |
| ESPN Australia live scoreboard | 50,834 | 7,340 | Reported second half, canonical Live/minute90 |
| ESPN Australia live summary | 399,133 | 5,103 | Auckland/Melbourne City full names and7 ordinary events |

Sources: [OpenLigaDB season](https://api.openligadb.de/getmatchdata/bl1/2026),
[current round](https://api.openligadb.de/getmatchdata/bl1),
[cup](https://api.openligadb.de/getmatchdata/dfb/2026/1),
[ESPN scoreboard](https://site.api.espn.com/apis/site/v2/sports/soccer/ger.1/scoreboard),
[scheduled date](https://site.api.espn.com/apis/site/v2/sports/soccer/ger.1/scoreboard?dates=20261017),
[finished summary](https://site.api.espn.com/apis/site/v2/sports/soccer/ger.1/summary?event=401884770),
[cup date](https://site.api.espn.com/apis/site/v2/sports/soccer/ger.dfb_pokal/scoreboard?dates=20260822),
[empty date](https://site.api.espn.com/apis/site/v2/sports/soccer/ger.1/scoreboard?dates=20261015),
[live date](https://site.api.espn.com/apis/site/v2/sports/soccer/aus.1/scoreboard?dates=20261017),
[live summary](https://site.api.espn.com/apis/site/v2/sports/soccer/aus.1/summary?event=401892161).
These mutable endpoints may now return different content. Host peaks stayed
within the unchanged24,576-byte allocator cap, with no residual allocations;
they are not C6 measurements. The summary cap remains524,288 bytes. New metadata
uses no separate retained JSON document. ESPN calendar dates are day markers;
its core calendar alone does not prove a whitelist or true neighboring kickoffs.
Keyed-provider real responses, suspension/no-kickoff live replies and automatic
coverage acquisition remain unverified. Parser success alone never proves that
an endpoint returned every fixture of the required interval.

### Historical coverage-binding replay — 2026-10-17

At that historical checkpoint, later replays used the raw metadata observer
and its production filter.
The OpenLigaDB relegation2025 body is3321B and contains two actual May21–25
kickoffs; only that interval is Complete. Its successful empty/full-season
endpoint does not establish arbitrary surrounding dates. The299845B league
season remains over262144B and cannot prove a gap. ESPN league/live/empty
calendar day whitelists validate metadata but do not establish adjacent UTC
fixture intervals; cup calendar shape also remains unproved. These spans remain
Partial. The host cup parser peak at this later filter/observer checkpoint is
12258B; it is not a target TLS/heap measurement. Real keyed-provider responses,
justified suspension and missing-kickoff bodies remain unverified.

### Current acquisition and operational guards

Current acquisition uses the shared streaming JSON Reader and typed provider
facts. It retains no filtered JSON document, stored provider body or persisted
schedule proof. A dedicated1-MiB full-season transport bound admits the captured
299845-byte OpenLigaDB season; parser workspace is bounded independently of
response length. League/cup fixture retention uses the previous/current/next
complete rounds plus the previous own game and next three own games. Full-season
index and admission passes are separate requests, each charged through the
normal scheduler and durable request budgets.

Current coverage binds stable competition/incarnation/source/season identities
in RAM and invalidates contradictory ordinary rows before display admission.
Extra discovery shares normal request limits and a persist-before-send guard:
one new run per UTC day, at most sixteen actual requests per run. Its36-byte
current-only NVS record contains two counters and the active credit-lifetime
marker. Route/season traversal and source fingerprints remain in RAM; reboot
starts traversal anew with remaining durable credits, and cannot refill a run
at midnight. A source change retires the current run and forfeits its remaining
credits before another source can send. ESPN has no extra discovery lane:
calendar markers support ordinary retention but cannot prove an empty interval,
so schedule-break evidence remains Unknown. A successful cup table response
with an explicitly empty primary collection is Complete, distinct from missing,
failed or unsupported data.

The adapter also declares logical dataset support. OpenLigaDB fixture goals and
ESPN summary events are supported; this integration maps no API-Football or
football-data.org events, which are explicitly Unavailable. Real keyed-provider
response/state acceptance and physical heap/stack reserves remain unverified;
controlled replays and Demo do not make incomplete sources complete.


### Typed response facts

Adapters emit compact provider values rather than internal store records. On C6,
match facts occupy20B (fixture input132B), table values6B (row input44B) and
shared event facts76B (event input80B). Only MatchStore adds directory/detail/pool
indices and observation history; canonical match/table/event capacities stay
unchanged. Parser event workspace is7,680B; the independently lazy response-detail
buffer is7,296B. When both coexist their requested payload is14,976B, excluding
allocator overhead, TLS and call stacks. This is allocation accounting, not a
measured device heap peak. Metadata and empty responses allocate no staging. The first actual event or
buffered table group allocates one workspace; only its active union member is
initialized. A denied allocation reports memory failure and cannot complete a
scope or replace a previous table. Anonymous-event hashing uses no staging.

Setup list adapters emit typed competition or team callbacks. Team identifiers
are checked positive uint32 at the provider token boundary and stay numeric
through the worker, application reader, HTTP and client draft. Original-name
streaming receives that same ID directly. Competition IDs/season text retain
their provider-specific validation; no full list/document tree is retained.

Automatic API-Football season resolution validates the `/leagues` envelope and
exact league ID, plus exactly one current valid year. Parameters, single-page
result count and empty errors are required before creating a season source.
A year alone cannot qualify fixtures or table work. The format is documented in
the provider's [current-season response example](https://www.api-football.com/news/post/how-to-get-standings-for-all-current-seasons)
(checked2026-10-18); controlled parser/planner regressions do not replace live
keyed-provider acceptance.

ESPN own-horizon acquisition reads both results and upcoming schedules with an
explicit season. Current public responses checked2026-10-18 for teams125/598
contain6results and28upcoming each; future bodies226416/225284B fit the1MiB
season limit. A single bounded selector combines both lists before retaining
one previous and three next games per distinct configured club. One index pass
per list precedes the two admission passes; every request yields to the normal
scheduler and its budgets. Repeated views of the same source/club share those
four requests; cost follows the same phase descriptor and club grouping.
