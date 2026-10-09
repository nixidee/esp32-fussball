# Canonical data model

> Status: **planned** (P4). No canonical data types or repository exist yet.
> Field names are proposals. Exact capacities, numeric widths, overflow policy
> and snapshot handover must be decided and budgeted before implementation.

All providers map their data into these structures. Nothing outside a provider
module sees provider-specific types. Canonical containers have fixed capacity;
bounded library allocations are measured separately. Software limits belong in
`include/app_config.h`.

## Validity, ownership and bounds
- Missing, null, malformed and out-of-range inputs remain unavailable; an
  unavailable score is not 0:0, and an unavailable kickoff is not epoch zero.
  A valid zero is retained as a real value. Kickoff, each score phase, team
  references and optional table/event values need explicit validity.
- Values derived from other fields carry provenance. Match status distinguishes
  `PROVIDER`, `ESTIMATED` and `UNKNOWN`; inferred phases never look confirmed.
  Stale but usable data retains its value, fetch time and stale indication.
  Presenters show an unknown placeholder or omit the unavailable field; they
  never manufacture a value to fill a view.
- IDs, references and bounded UTF-8 text belong to the published model, not
  to temporary JSON/parser memory. Truncation stops at a complete UTF-8
  character. Text fallback and glyph coverage are verified with real club and
  player names, including umlauts.
- Goals, counts and ordinary minutes are non-negative; goal difference and
  points can require signed values. Accepted numeric ranges and storage widths
  are fixed before implementation; conversion rejects overflow or an invalid
  range instead of wrapping or substituting zero.
- Before allocating the model, set limits for competitions, teams, fixtures,
  standing rows, events and text bytes, plus an explicit overflow outcome per
  collection. Record per-record size/padding and capacity multiplication for
  every live snapshot and working buffer.

## Entities

### Competition
| Field | Type | Notes |
|---|---|---|
| `id` | provider-scoped id | e.g. OpenLigaDB `bl1` |
| `name`, `short_name` | text | |
| `kind` | enum `LEAGUE`, `CUP` | |
| `season` | provider-scoped season identifier | retain the provider's identity; not inferred from calendar year |
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
| `home`, `away` | valid Team ref or unavailable | references owned by the snapshot |
| `kickoff_utc` | valid epoch seconds or unavailable | UTC only |
| `status` | enum `SCHEDULED`, `LIVE_1H`, `HALF_TIME`, `LIVE_2H`, `EXTRA_TIME`, `PENALTIES`, `FINISHED`, `POSTPONED`, `CANCELLED`, `UNKNOWN` + status source | exceptional/abandoned states are resolved before the provider mapper |
| `minute` | int + `minute_source` (`PROVIDER`, `ESTIMATED`, `NONE`) | estimated values are displayed as estimated |
| `score` | phase scores with independent validity | half-time, regulation, after extra time and shootout, see contract below |
| `last_score_change_utc` | valid epoch seconds or unavailable | repository change detection; first-load baseline is not a new goal |
| `events` | bounded list of `MatchEvent` | retention/overflow policy decided before allocation |
| `successful_fetch_utc` | valid epoch seconds or unavailable | local receipt of a successful response; separate from provider change time |
| `provider_last_changed_utc` | valid epoch seconds or unavailable | provider-reported change time; may be absent or unchanged on a new fetch |

### MatchEvent
| Field | Type |
|---|---|
| `identity`, `revision` | deterministic identity and correction information; exact representation decided before mapping |
| `type` | enum `GOAL`, `OWN_GOAL`, `PENALTY_GOAL`, `PENALTY_MISSED`, `YELLOW`, `YELLOW_RED`, `RED`, `SUBSTITUTION`, `INJURY`, `VAR` |
| `minute`, `extra_minute` | valid non-negative number or unavailable |
| `team`, `beneficiary` | actor's team and scoring beneficiary separately, if known |
| `player` | text (bounded) |
| `score_after` | for goal types |

Card, substitution and `INJURY` types are kept in the model for richer
providers (decided 2026-10-08): with OpenLigaDB only goal types occur. The
2026-10-08 research found no free source for `INJURY`; it is not displayed
for now.

### Score and event contract
- Regulation totals, totals after extra time and shootout tallies are separate;
  shootout goals are not added to the displayed match goal total. The principal
  scoreboard uses the valid score for the reported match phase; an unavailable
  phase stays unknown. Provider-specific result meanings are mapped by semantic
  result kind, not an assumed numeric result ID. Unknown kinds remain unknown.
- The repository reconciles events by deterministic identity, including
  corrections, removed goals and changed scores. An own-goal actor and the team
  benefiting from the goal must remain distinguishable. Exact identity,
  revision and removal rules are decided before the relevant mapper/store.
- Initial fetch and reconnect establish a baseline and do not replay historic
  goal effects. Subsequent confirmed additions/corrections are reconciled once;
  reordering or duplicate responses must not repeatedly trigger effects.
- Substitution and VAR types remain available for richer providers. Their
  detailed fields and capacity cost are designed before adding that provider;
  a type name alone does not establish support for its full event data.

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
  `matchIsFinished` and current time. Exact rules in P4.4; elapsed time alone
  cannot establish a confirmed live phase or final result. Interrupted,
  delayed and stale/unknown states need explicit policy.
- **Minute without provider minute**: estimated from kickoff with a fixed
  half-time break; marked `ESTIMATED` and displayed as estimated (e.g.
  `~67'`).
- **Position without provider field**: order of the table response.

## Snapshot
The repository publishes an immutable snapshot: own club, active competitions,
own match (if any), relevant fixtures, table of the own league, freshness/errors
per source. One writer owns publication. A reader's references remain valid for
its entire declared lifetime; a buffer cannot be reused merely because it is
no longer the newest snapshot. Readers take short protected copies of the data
they need (no leases on snapshot slots); copy sizes and lock times are measured
when the repository is implemented.

Requests and their results carry the settings generation (club, competition,
provider selection). Results for an old generation cannot publish into a new
selection. Queues have fixed limits and explicit coalescing/rejection policy;
commands that require acknowledgement cannot be silently dropped.

Relevant fixtures use round identity **and** a bounded date horizon so an
earlier-round catch-up game is not omitted. The normal matchday window stays
30 minutes before the first relevant kickoff to 30 minutes after the last
confirmed finish, with both offsets configurable. Delayed, postponed,
abandoned or stale/unknown fixtures need a bounded window-exit policy without
inventing a final result. Horizon and exceptional-case rules are decided before
the scheduler/resolver. Test season rollover and off-season separately.

## Multiple providers
Provider-scoped IDs remain the initial identity scheme. Cross-provider
composition requires the mapping, provenance and conflict contract in
[DATA_PROVIDERS.md](DATA_PROVIDERS.md#combining-providers-before-optional-provider-routing).

## Resource and acceptance requirements
Implementing validity/provenance flags, event revisions, mappings and snapshot
handover has a cost: measure their record sizes, all resident
copies, synchronization storage and parser/working peaks before introduction.

Host fixtures must exercise missing/null values, valid zeros, malformed/range
errors, bounded UTF-8, regulation/extra-time/shootout results, duplicate/corrected
and removed events, initial/reconnect baselines, clock changes, catch-up games,
missing final status and season transitions. Repository tests cover delayed
readers, repeated publications, full queues and a settings change while a
request is in flight. Device measurements then validate the complete model
with representative maximum data and permitted concurrent operations.
