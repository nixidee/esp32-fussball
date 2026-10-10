# Canonical football data

Implementation: `components/football/include/football_model.h`, provider
mappers and `football_service.cpp`. Current C6 source compiles; fixture,
concurrency and maximum-load acceptance remain open.

## Identity, values and ownership

A provider ID is meaningful only with its provider and configured route.
Settings own three competition/season/club routes; matches refer to a route
index. There is no separate heap-allocated competition graph. Teams, names,
scores and events are copied into the snapshot and never borrow JSON storage.

`Value<T>` contains a value and source: None, Estimated or a confirmed
provider. None means unavailable; genuine zero remains valid. Kickoff and
provider-change instants use UTC seconds. Fetch and score-change times use
monotonic milliseconds, so wall-clock corrections do not reorder freshness
or recent-score sorting. A finished time observed locally is explicitly estimated.

IDs accept at most 32 bytes and reject overflow or embedded NUL. Display names
accept 64 bytes, short names 24; displayed prefixes end at complete UTF-8
characters. Unknown glyphs use the font fallback. Numeric conversion accepts
integral JSON numbers and decimal strings, rejects booleans, null, fractions,
overflow and invalid ranges. Goals are 0–99; minutes 0–240; added minutes
0–120. Table points and goal difference can be negative within ±10000.

## Fixed records

| Record | Content | C6 compiler size |
|---|---|---|
| Team | provider, owned ID, name, short name | 124 B |
| Match | provider/route/ID, numeric and textual round, teams, timestamps, phase/minute, five scores, event indices, own flag | 456 B |
| MatchEvent | provider, 32-bit identity/revision, type, minute/added time, actor/beneficiary, player/in/out/detail, score after, removed/shootout flags | 224 B |
| Standing | owned team, own flag, position and eight statistics with validity | 162 B |
| Selection | owned ID, provider season, name | 131 B |
| Snapshot | 32 matches, 32 table rows, 32 events, counts, generation/revision/freshness/errors | 27168 B |

Limits live in `app_config.h`. Each match references at most 12 events in the
shared 32-entry pool. Rich events are retained for the tracked club across its
competitions; conference matches use score changes. After 12 events the oldest
entry for that match is reused and truncation is flagged. A full global pool
flags truncation without overflowing. Removal detection is disabled for
truncated lists, so eviction cannot pretend that a goal was withdrawn.

Relevant match/table overflow rejects the complete new publication and keeps
the previous snapshot with an error/stale indication. Duplicate fixture IDs
within a route are ignored. Selection uses a separate 64-entry page and an
explicit more flag; it is not an unbounded team cache.

## Match phases and scores

Phases: Unknown, Scheduled, Live, HalfTime, ExtraTime, Penalties, Finished,
Postponed, Cancelled and Abandoned. Status and minute have independent
provenance. Missing status remains unknown or explicitly estimated; elapsed
time never proves a final result.

Principal goals, half-time, regulation totals, totals after extra time and
shootout tallies are separate. Shootout goals are never added to principal
goals. Incomplete pairs display an unknown score.

OpenLigaDB maps semantic `resultTypeKind`, not numeric result IDs. Its result
after penalties includes the earlier goals; the mapper subtracts the known
after-extra-time total to obtain a shootout tally. If that base is unavailable
or subtraction would be negative, the tally remains unavailable. Unknown
result kinds are not reassigned by name/order. football-data's additional
extra-time goals are added to regulation; its shootout-inclusive fullTime
is not used as ordinary goals. ESPN's separate shootoutScore is retained.

OpenLigaDB lacks a live clock: before kickoff status is estimated Scheduled;
after kickoff the minute uses a 15-minute halftime allowance and is marked
with `~`. After four hours without a confirmed final status phase becomes
Unknown and the bounded window expires. Postponed/cancelled/abandoned fixtures
are excluded from active windows.

## Events and corrections

Types: Goal, OwnGoal, PenaltyGoal, PenaltyMissed, Yellow, YellowRed, Red,
Substitution, Injury and VAR. Actual availability depends on the response and
provider coverage. Actor and beneficiary are separate; an unknown reference
stays empty. OpenLigaDB beneficiaries are derived only when consecutive
reported scores identify a single increment.

A supplied event ID is preferred. Otherwise a deterministic hash of type,
minute, added time, team/player/substitution and shootout identity is used.
Revision hashes include score/detail/player changes. Events missing from a
complete later list become one-publication removal records. These 32-bit
hashes are bounded representations; collision/correction fixtures still need
tests. Richer provider event semantics require keyed/recorded acceptance.

The first fetch, reconnect, settings generation change and time-change
notification establish a baseline. They do not replay historical goals.
Later principal-value changes update monotonic score-change time; switching
provenance alone does not count as a score change.

## Relevant fixtures and matchday windows

Own fixtures are retained within seven days on either side of current UTC.
OpenLigaDB additionally retains the provider's current round. Other providers
retain conference fixtures within 36 hours. Filtering happens while streaming
a response; discarded bytes still count toward transport limits.

The configured competition conference windows span the first kickoff through the
last finish of each relevant round, including gaps between fixtures. Round
number is preferred, then supplied round text; without either, UTC date is
the grouping boundary. Each fixture's unknown end is capped at kickoff plus
240 minutes. Configurable before/after offsets default to 30 minutes.
Own cup/catch-up matches have their own windows and can select Own match even
outside the league window. This date fallback is a bounded implementation
choice, not a claim that every provider supplies complete round metadata.

Season comes from the selected provider. OpenLigaDB resolves an empty season
from current match metadata and caches it for six hours. No rule guesses a
season from the local calendar. Season rollover, empty competitions and delayed
fixtures remain required acceptance cases.

## Publication and readers

One provider worker owns publication. Two static snapshot slots and one reusable
route scratch snapshot cost **81504 B** together. Runtime constructors place
them in BSS rather than embedding mostly empty images in flash.

A reader executes a short callback under the repository mutex and copies into
its own fixed view/status model; it performs no I/O, LVGL work, allocation or
blocking calls there. It retains no snapshot reference. The presenter view
model is 1416 B on C6. The writer may reuse the old slot after publication
because every read has already released its protected copy.

Every request carries settings generation and network epoch. Changed settings,
network loss/reconnect or deadline expiration invalidate work before publication.
A failed route rejects the candidate as a whole; the previous complete snapshot
remains available with stale/error state. A stale age of 15 minutes is also
reported independently of request failure.

Cross-provider fields use explicit fixture mappings and precedence from
[DATA_PROVIDERS.md](DATA_PROVIDERS.md). Valid primary values win; a fallback
never guesses a common ID from similar names.

## Acceptance boundary

Compiler sizes establish fixed storage only. No current host fixture suite,
slow-reader stress, event reconciliation, source-conflict, clock/season
transition or device maximum-load run has been performed for this model.
Those checks must include all three resident snapshots, JSON/TLS, task stacks,
JPEG redraws and local Web access; earlier integration evidence does not
certify this candidate.
