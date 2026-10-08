# Data providers

> Research state 2026-10-08. “Verified” = tested with real requests or read
> from the provider's own data; “unverified” = from third-party sources, must
> be re-checked before implementation.

## Requirement
German competitions down to Regionalliga (leagues and cups), live scores,
tables. Free provider as default; paid providers optional with API key.

## Comparison
| Provider | Cost / auth | German coverage | Live | Events | Table | Status |
|---|---|---|---|---|---|---|
| **OpenLigaDB** | free, no key | BL, 2. BL, 3. Liga, DFB-Pokal, Regionalliga Nord, Nordost, Bayern, UCL (season 2026). **No RL West / Südwest.** | yes (community-entered), cheap change check `getlastchangedate` | **goals only** (minute, scorer, penalty, own goal) | yes | verified |
| **API-Football** (api-sports.io) | key; free 100 req/day, 10 req/min; paid from ~$19/month | claims broad coverage; Regionalliga **not confirmed** | yes, elapsed minute | goals, cards, subs, VAR | yes | unverified |
| **ESPN** (unofficial site API) | free, no key, **no contract / may change anytime** | `ger.1`, `ger.2`, `ger.dfb_pokal` respond; 3. Liga / Regionalliga slugs unknown | yes, status + clock | details incl. cards (format to verify) | standings endpoint to verify | partly verified |
| **football-data.org** | key; free 10 req/min | free tier: Bundesliga only (no 2./3. Liga, no DFB-Pokal) | yes | limited in free tier | yes | unverified |

No free source was found that delivers **injuries** as match events.

## Decision (ADR-005, accepted 2026-10-08)
1. **OpenLigaDB as default** — only free, keyless source verified down to
   Regionalliga. Limits: no cards/subs, no match clock → minute estimated
   and shown as `~67'`.
2. **API-Football as optional keyed provider** — richer events; budget of
   100 requests/day requires strict scheduling (e.g. live polling only for the
   own match). Coverage of Regionalliga to verify with a key before implementation.
3. **ESPN as optional opt-in** — clearly labelled unofficial.
4. football-data.org — low value for this project with free tier; later.

## Competitions and clubs in scope (decided 2026-10-08)
- German leagues down to Regionalliga and the DFB-Pokal as listed above.
- UEFA competitions (incl. Champions League) wanted; shown optionally, like
  the DFB-Pokal.
- Clubs in use / tested: Rot-Weiss Essen, FC Bayern München, Hertha BSC,
  Borussia Mönchengladbach, 1. FC Union Berlin, Hallescher FC. Their
  current leagues and provider coverage are verified before use (P9).
- Club selection: per provider from its team list; on a provider switch the
  device suggests the club by name. The structure allows adding a
  canonical club mapping later with small changes.

## OpenLigaDB details (verified)
- Base URL `https://api.openligadb.de`.
- Endpoints used:
  - `GET /getavailableleagues` — competitions and shortcuts.
  - `GET /getmatchdata/{league}/{season}/{matchday}` — matches of a round
    (≈ 7 KB for a Bundesliga round).
  - `GET /getlastchangedate/{league}/{season}/{matchday}` — timestamp; poll
    this and only fetch match data when it changed.
  - `GET /getbltable/{league}/{season}` — table (order = position).
- Team icons: Wikimedia URLs (SVG/large PNG) — not decoded on the device.
- Fair use: third-party guidance of ≤ 1 request per league every 30–60 s
  during live play (unverified, to confirm before implementation).

## Polling strategy (planned, P4.6)
| State | Interval (proposal) |
|---|---|
| No relevant match today | table + fixtures a few times per day |
| Pre-match window | every few minutes |
| Live | change check every 30–60 s (OpenLigaDB); keyed providers per budget |
| Post-match | a few checks until final result is confirmed |

Exact values go into `include/app_config.h` (P2.1) once the provider limits are confirmed.

## Adding a provider
1. Implement the provider interface (fetch competitions, teams, round,
   table, live updates) and map to [DATA_MODEL.md](DATA_MODEL.md).
2. Declare capabilities and rate limits.
3. Add recorded JSON fixtures and host tests (`test/fixtures/<provider>/`).
4. Add key/settings fields (if any) to the settings schema and
   `secrets.h.example`.
5. Update this file and README.
