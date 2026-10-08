# Canonical data model

> Status: **planned** (P4.1). Field names are a proposal; capacities are
> placeholders until measured on the device (P4).

All providers map their data into these structures. Nothing outside a provider
module sees provider-specific types. All containers have fixed capacity
(no heap churn on the device).

## Entities

### Competition
| Field | Type | Notes |
|---|---|---|
| `id` | provider-scoped id | e.g. OpenLigaDB `bl1` |
| `name`, `short_name` | text | |
| `kind` | enum `LEAGUE`, `CUP` | |
| `season` | int | e.g. 2026 |
| `current_round` | int + round name | matchday number or cup round name |

### Team
| Field | Type | Notes |
|---|---|---|
| `id` | provider-scoped id | |
| `name`, `short_name`, `code` | text | `code` = 3-letter if available, else derived |

### Match
| Field | Type | Notes |
|---|---|---|
| `id` | provider-scoped id | |
| `competition_id`, `round` | | |
| `home`, `away` | Team ref | |
| `kickoff_utc` | epoch seconds | |
| `status` | enum `SCHEDULED`, `LIVE_1H`, `HALF_TIME`, `LIVE_2H`, `EXTRA_TIME`, `PENALTIES`, `FINISHED`, `POSTPONED`, `CANCELLED`, `UNKNOWN` | providers without status → derived (see below) |
| `minute` | int + `minute_source` (`PROVIDER`, `ESTIMATED`, `NONE`) | estimated values are displayed as estimated |
| `score` | home/away + half-time, after extra time, penalties | |
| `last_score_change_utc` | epoch seconds | used for sorting in conference view |
| `events` | bounded list of `MatchEvent` | newest kept on overflow |
| `updated_utc` | epoch seconds | freshness |

### MatchEvent
| Field | Type |
|---|---|
| `type` | enum `GOAL`, `OWN_GOAL`, `PENALTY_GOAL`, `PENALTY_MISSED`, `YELLOW`, `YELLOW_RED`, `RED`, `SUBSTITUTION`, `INJURY`, `VAR` |
| `minute`, `extra_minute` | int |
| `team` | home/away |
| `player` | text (bounded) |
| `score_after` | for goal types |

Card, substitution and `INJURY` types are kept in the model for richer
providers (decided 2026-10-08): with OpenLigaDB only goal types occur. No
current free provider delivers `INJURY`; it is not displayed for now.

### StandingRow
| Field | Type |
|---|---|
| `position` | int (derived from order if provider has none) |
| `team` | Team ref |
| `played`, `won`, `drawn`, `lost` | int |
| `goals_for`, `goals_against`, `goal_diff`, `points` | int |

### Provider capabilities
Flags per provider (and per competition if needed): `HAS_LIVE`,
`HAS_STATUS`, `HAS_MINUTE`, `HAS_CARDS`, `HAS_SUBS`, `HAS_TABLE`,
`HAS_CUPS`, `NEEDS_KEY`, plus rate limits (per minute / per day).
Presenters use capabilities to decide what to show (e.g. no card section if
`HAS_CARDS` is false) — never provider names.

## Derivations (provider has no field)
- **Status without provider status** (OpenLigaDB): from kickoff time,
  `matchIsFinished` and current time. Exact rules in P4.4.
- **Minute without provider minute**: estimated from kickoff with a fixed
  half-time break; marked `ESTIMATED` and displayed as estimated (e.g.
  `~67'`).
- **Position without provider field**: order of the table response.

## Snapshot
The repository publishes an immutable snapshot:
own club, active competitions, own match (if any), current round matches of
relevant competitions, table of the own league, freshness/errors per source.
