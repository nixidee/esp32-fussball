// Compact football data store (OQ-227): the matches of every competition,
// all league tables, a team directory and bounded match details. Own-club
// details retain eight protected slots and 48 events; explicitly requested
// ordinary Singles have another eight slots and 48 events in the same pools.
// Rows hold numeric provider ids and typed directory indices instead of
// names; the shared directory alone holds original compact full/short
// fallbacks. A complete acquisition retains three rounds plus the nearest own
// fixtures; a failed acquisition never prunes known facts. Plain fixed storage:
// no heap, no locking (the owner serialises access). Pure, so the native tests
// compile it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "app_config.h"
#include "football_model.h"

namespace football {
namespace retention {
struct Selection;
}

// Team/match pools have independent 16-bit references. Small label/detail/
// event pools keep their byte sentinel; values (goals/minute) are independent.
using TeamIndex = uint16_t;
using MatchIndex = uint16_t;
inline constexpr TeamIndex kNoTeam = UINT16_MAX;
inline constexpr MatchIndex kNoMatch = UINT16_MAX;
inline constexpr uint8_t kNoIndex = 0xff;
// Bits of StoredMatch::flags.
inline constexpr uint8_t kMatchEventsTruncated = 0x02;
// Events were lost to this match's protected event bank (kNoticeEvents
// stays until no detail match of the competition carries it, OQ-311).
inline constexpr uint8_t kMatchEventsPoolCut = 0x04;
// Marked only by the current authoritative fixture refresh. A failed run
// never prunes; the next run clears these bits before receiving new facts.
inline constexpr uint8_t kMatchRefresh = 0x08;
// Bits of StoredMatch::estimated: the value was derived by the device, not
// reported by the provider.
inline constexpr uint8_t kEstimatedPhase = 0x01;
inline constexpr uint8_t kEstimatedMinute = 0x02;
inline constexpr uint8_t kEstimatedFinish = 0x04;

struct StoredMatch {
  uint32_t id = 0;             // provider match id (never 0)
  uint32_t kickoff = 0;        // UTC seconds, 0 = unknown
  uint32_t finished_at = 0;    // UTC seconds of the final whistle, 0 = unknown
  uint32_t score_changed = 0;  // monotonic seconds of the last change, 0 = none
  // First confirmed provider-running observation of this live episode.
  // UTC for the current runtime; never a substitute kickoff or a checkpoint.
  uint32_t first_live_observed_utc = 0;
  uint8_t competition = 0;  // stable competition id (1-255)
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  TeamIndex home = kNoTeam, away = kNoTeam;  // team directory
  uint8_t round = 0;                         // provider round number, 0 = none
  uint8_t label = kNoIndex;                  // round label pool
  Phase phase = Phase::kUnknown;
  uint8_t minute = kNoValue, added = kNoValue;
  Period period = Period::kUnknown;
  Goals principal, shootout;
  uint8_t flags = 0, estimated = 0;
  uint8_t detail = kNoIndex;  // common detail slot, independent of ownership
};

struct StoredTeam {
  uint32_t id = 0;  // provider team id, 0 = free entry
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  Text<cfg::kTeamNameBytes> name;
  Text<cfg::kTeamShortBytes> short_name;
};

struct StoredRow {
  TeamIndex team = kNoTeam;
  uint8_t competition = 0;
  uint8_t position = kNoValue, played = kNoValue;
  uint8_t group = kNoIndex;  // ordinal within this competition table
  int16_t difference = kNoNumber, points = kNoNumber;
};

// Source facts are runtime-only. Unknown/missing and a successful empty table
// remain distinct; Partial retains the previous facts after an interrupted run.
enum class DatasetState : uint8_t {
  kUnknown,
  kPartial,
  kComplete,
  kUnavailable
};
inline constexpr const char* datasetStateKey(DatasetState state) {
  switch (state) {
    case DatasetState::kUnknown: return "unknown";
    case DatasetState::kPartial: return "partial";
    case DatasetState::kComplete: return "complete";
    case DatasetState::kUnavailable: return "unavailable";
  }
  return "unknown";
}
struct DatasetStatus {
  int64_t last_attempt_ms = 0, last_success_ms = 0;
  uint16_t http = 0;
  Cause cause = Cause::kNone;
  DatasetState state = DatasetState::kUnknown;
};
struct DatasetSummary {
  int64_t last_success_ms = 0, failed_attempt_ms = 0;
  uint16_t http = 0;
  Cause cause = Cause::kNone;
};
struct StoredTableGroup {
  uint8_t competition = 0, ordinal = 0, label = kNoIndex;
};
struct StoredRound {
  uint16_t matches = 0;
  uint8_t competition = 0, number = 0, label = kNoIndex;
  DatasetState state = DatasetState::kUnknown;
};

// Only the pool owner adds a slot to the shared event facts.
struct StoredEvent : EventFacts {
  uint8_t slot = kNoIndex;
};

// Bounded events shared by own and ordinary matches; team labels have one
// owner.
struct MatchDetails {
  uint32_t match = 0;  // match id, 0 = free slot
  uint8_t competition = 0;
  uint8_t event_count = 0;
  std::array<uint8_t, cfg::kMatchEvents> events{};  // event pool indices
};

// Bits of the per-competition notices: data the store could not hold
// (cause E19 store_full, OQ-310). Each kind is cleared when it is next
// stored completely (OQ-311): matches and their teams by a complete
// fixtures run (beginRun()/settleRun(), a live run only adds), the table and
// its teams by a replaced table, events when no detail match of the
// competition carries kMatchEventsPoolCut.
inline constexpr uint8_t kNoticeMatches = 0x01;     // a match did not fit
inline constexpr uint8_t kNoticeTable = 0x02;       // a table did not fit
inline constexpr uint8_t kNoticeMatchTeams = 0x04;  // directory full (match)
inline constexpr uint8_t kNoticeEvents = 0x08;      // an event did not fit
inline constexpr uint8_t kNoticeTableTeams = 0x10;  // directory full (table)
inline constexpr uint8_t kNoticeTeams = kNoticeMatchTeams | kNoticeTableTeams;
// Notices a complete fixtures run settles.
inline constexpr uint8_t kNoticeFixtures = kNoticeMatches | kNoticeMatchTeams;

// Time of an update: UTC and monotonic seconds.
struct Clock {
  int64_t utc = 0;
  uint32_t mono_s = 0;
};

// Local civil dates in the currently applied timezone. The callback resolves
// the offset separately at each instant, including historical DST changes.
// A fixed fallback supports pure callers without a timezone adapter.
// No state is retained.
struct DateContext {
  using OffsetAt = int32_t (*)(int64_t utc, void* context);
  OffsetAt offset_at = nullptr;
  void* context = nullptr;
  int32_t fallback_offset_s = 0;
};
// Local civil date as days since 1970-01-01, using the offset at this
// particular instant. Shared by result retention and phase date clusters.
int32_t civilDay(int64_t utc, const DateContext& dates);
// A finished result stays through the local day following its finish day.
// Unknown finish or invalid current UTC does not establish a result hold.
bool resultHeld(const StoredMatch& match, int64_t utc,
                const DateContext& dates);

class MatchStore {
 public:
  // Out of line on purpose: an implicit constexpr constructor would place a
  // static store in initialised data, copied from flash at boot.
  MatchStore();
  void clear();

  // Directory index of a team, added or with prepared display labels updated;
  // kNoTeam for id 0 or a full directory (also notes kNoticeTableTeams for
  // 'competition' while its table is staged, else kNoticeMatchTeams).
  TeamIndex team(uint8_t competition, cfg::Provider provider, uint32_t id,
                 std::string_view name, std::string_view short_name);
  // Pure/demo callers without a separate provider abbreviation use the same
  // original text for both labels, clipped independently to their byte limits.
  TeamIndex team(uint8_t competition, cfg::Provider provider, uint32_t id,
                 std::string_view name) {
    return team(competition, provider, id, name, name);
  }
  // Applies a club preference to retained source facts. Scores, timing,
  // rounds and tables stay; detail/event ownership moves between the existing
  // banks where capacity permits. No fetch, allocation or source reset.
  void selectClub(uint8_t competition, cfg::Provider provider, uint32_t club,
                  const Clock& clock);
  // Membership is derived from the current source preference and team keys.
  // Neither matches nor table rows retain an independent ownership bit.
  void ownSides(const StoredMatch&, bool& home, bool& away) const;
  bool isOwn(const StoredMatch&) const;
  bool isOwn(const StoredRow&) const;
  // Pool index of a round label; kNoIndex for an empty text or a full pool.
  uint8_t label(std::string_view text);

  // Stores 'incoming' (teams and label already interned), replacing the
  // stored match with the same competition, provider and id. Kept from the
  // stored match: the time of the last score change (set to 'clock.mono_s'
  // when both scores are known and differ) and the final-whistle time. A
  // match first seen finished gets 'clock.utc' as estimated final whistle,
  // or the expected end when it was never seen running. An own match gets
  // a protected detail slot; a full own bank gives the
  // slot of the own match farthest from now when this one is nearer. A
  // full store makes room by removing the other club's match farthest from
  // now when this one is own or nearer; nullptr (kNoticeMatches) when it
  // does not fit. An ordinary match retains an explicitly requested detail
  // slot; it does not allocate one automatically. The list stays sorted by
  // kickoff.
  StoredMatch* upsert(const StoredMatch& incoming, const Clock& clock);
  // Ensures details for this explicit target, without changing club selection.
  // Own targets use their protected nearest-match bank. Ordinary targets use
  // only their separate bank, preferring an older target of the same
  // competition before its farthest target. Ordinary upsert() never allocates
  // details by itself; it refreshes a slot previously requested here.
  StoredMatch* ensureDetail(uint8_t competition, cfg::Provider provider,
                            uint32_t id, const Clock& clock);
  // Re-estimates phase and minute of every match without a provider phase.
  void estimateAll(int64_t utc);
  // Removes every match, table row and notice of 'competition'.
  void removeCompetition(uint8_t competition);
  // Frees teams and round labels no match or row refers to.
  void collect();

  // Replaces the table of 'competition' in three steps, so a response that
  // breaks off keeps the stored table: begin, append the rows in order,
  // commit. The rows are staged in a buffer of their own; the stored table
  // is untouched until commitTable(true), which replaces it with the staged
  // rows (including a successfully empty table) unless a row did not fit the
  // buffer or the new
  // table does not fit the row pool; otherwise the stored table stays. A row
  // that does not fit the buffer and a table that does not fit the pool note
  // kNoticeTable; a replaced table clears it. appendRow() is false when the
  // row was not staged.
  void beginTable(uint8_t competition);
  bool appendRow(const StoredRow& row);
  bool appendGroup(std::string_view key, std::string_view label,
                   uint8_t ordinal = kNoIndex);
  std::size_t groupCount() const { return group_count_; }
  const StoredTableGroup& group(std::size_t at) const { return groups_[at]; }
  DatasetState tableState(uint8_t competition) const;
  enum class Dataset : uint8_t { kFixtures, kTable, kEvents };
  DatasetState datasetState(uint8_t competition, Dataset dataset) const;
  void datasetResult(uint8_t competition, Dataset dataset, bool complete);
  // Source capability only: unavailable carries no fabricated HTTP failure.
  // Reapplying supported capabilities preserves known facts and failures.
  void datasetAvailable(uint8_t competition, Dataset dataset, bool available);
  DatasetStatus datasetStatus(uint8_t competition, Dataset dataset) const;
  // Existing capacity/provider-maintenance notices for this data type only.
  Cause datasetNotice(uint8_t competition, Dataset dataset, int64_t utc) const;
  DatasetSummary datasetSummary(uint8_t competition) const;
  // attempt_ms is zero for requests refused before the actual HTTP send.
  // A successful intermediate request does not erase an earlier run failure.
  void recordDataset(uint8_t competition, Dataset dataset, int64_t attempt_ms,
                     int64_t completed_ms, bool complete, Cause cause,
                     uint16_t http = 0);
  void roundResult(uint8_t competition, uint8_t number, uint8_t label,
                   uint16_t matches, bool complete);
  // Prune only after a complete authoritative fixture acquisition. The UI
  // result hold remains available beyond this stored union.
  void retain(uint8_t competition, const retention::Selection&, int64_t utc,
              DateContext dates = {});
  void beginFixtureRun(uint8_t competition);
  void markFixture(uint8_t competition, cfg::Provider, uint32_t match);
  std::size_t roundCount() const { return round_count_; }
  const StoredRound& round(std::size_t at) const { return rounds_[at]; }
  bool commitTable(bool complete);
  std::size_t stagedRows() const { return staged_rows_; }

  // Replaces the events of detail match 'match' in three steps: begin, add in
  // order, end. When neither the previous nor the new list was cut, a
  // previous event whose identity is missing from the new list stays as a
  // removal marker after the new ones. More than
  // kMatchEvents events keep the latest ones; a full pool drops the rest.
  // Both set kMatchEventsTruncated. endEvents(false), for a response that
  // broke off, keeps every previous event and adds the new ones after them.
  // 'detail' events come from the detail request (kEventDetail): the detail
  // and the list events each replace only their own events, the other
  // origin's stay ahead of the new list; when the slot is full detail
  // events give way first. A full pool gives this match the oldest event of
  // the match farthest from 'utc' in the same own/ordinary bank when this
  // one is nearer (OQ-292); that match is marked truncated and its
  // competition notes kNoticeEvents. 'utc' 0 = unknown, nothing is taken.
  // Events lost to the full pool mark the match kMatchEventsPoolCut until
  // its next complete list keeps every event (endEvents(true)).
  // False if 'match' has no slot.
  bool beginEvents(const StoredMatch& match, bool detail = false,
                   int64_t utc = 0);
  void addEvent(const EventFacts& event);
  void endEvents(bool complete = true);

  std::size_t matchCount() const { return match_count_; }
  const StoredMatch& match(std::size_t i) const { return matches_[i]; }
  const StoredMatch* find(uint8_t competition, cfg::Provider provider,
                          uint32_t id) const;
  const StoredTeam& teamAt(TeamIndex i) const { return teams_[i]; }
  StoredTeam& teamAt(TeamIndex i) { return teams_[i]; }
  const Text<31>& labelAt(uint8_t i) const { return labels_[i]; }
  std::size_t rowCount() const { return row_count_; }
  const StoredRow& row(std::size_t i) const { return rows_[i]; }
  bool hasDetail(const StoredMatch& match) const;
  const MatchDetails& detailAt(uint8_t i) const { return details_[i]; }
  const StoredEvent& eventAt(uint8_t i) const { return events_[i]; }
  uint8_t notices(uint8_t competition) const;
  // True when a match of 'competition' kicked off more than
  // cfg::kUnmaintainedAfterS before 'utc' and is neither over (finished,
  // postponed, cancelled, abandoned) nor has a result: its provider does not
  // maintain the competition (OQ-281).
  bool unmaintained(uint8_t competition, int64_t utc) const;
  // Sets or clears notice 'bits' of 'competition'.
  void flag(uint8_t competition, uint8_t bits, bool on);
  // A run that stores every item of notice kinds 'bits' (a fixtures run:
  // kNoticeFixtures) starts; settleRun() after it completed replaces those
  // bits by the ones noted since. A run that fails does not settle: its
  // notes add to the earlier ones.
  void beginRun(uint8_t competition, uint8_t bits);
  void settleRun(uint8_t competition, uint8_t bits);
  std::size_t teamCount() const;

 private:
  struct SourceStatus {
    uint32_t club = 0;
    uint8_t competition = 0;
    cfg::Provider provider = cfg::kProviderDefault;
    std::array<DatasetStatus, 3> datasets{};
  };
  SourceStatus* source(uint8_t competition);
  const SourceStatus* source(uint8_t competition) const;
  bool selected(const SourceStatus&, TeamIndex team) const;
  int matchIndex(uint8_t competition, cfg::Provider provider,
                 uint32_t id) const;
  void removeMatch(std::size_t i);
  void freeSlot(uint8_t slot);
  uint8_t detailSlot(const StoredMatch& match, int64_t utc, bool requested);
  void note(uint8_t competition, uint8_t bits);
  std::size_t sortedPosition(const StoredMatch& match) const;
  void dropRows(uint8_t competition);
  void dropGroups(uint8_t competition);
  int takeEvent();
  StoredMatch* detailRow(uint8_t slot);
  void settleEvents(uint8_t competition);
  StoredMatch* place(StoredMatch m, std::size_t from);

  std::array<StoredMatch, cfg::kStoreMatches> matches_{};
  std::array<StoredTeam, cfg::kStoreTeams> teams_{};
  std::array<StoredRow, cfg::kStoreTableRows> rows_{};
  std::array<StoredRow, cfg::kStoreTableStageRows> stage_{};
  std::array<MatchDetails, cfg::kStoreDetailMatches> details_{};
  std::array<StoredEvent, cfg::kStoreEvents> events_{};
  std::array<Text<31>, cfg::kStoreRoundLabels> labels_{};
  std::array<StoredRound, cfg::kStoreRounds> rounds_{};
  std::array<StoredTableGroup, cfg::kStoreTableGroups> groups_{};
  std::array<StoredTableGroup, cfg::kStoreTableGroups> stage_groups_{};
  std::array<SourceStatus, cfg::kCompetitionMax> sources_{};
  uint8_t round_count_ = 0, group_count_ = 0, staged_groups_ = 0;
  struct Notice {
    // 'noted': bits noted since beginRun().
    uint8_t competition = 0, bits = 0, noted = 0;
  };
  std::array<Notice, cfg::kCompetitionMax> notices_{};
  // Event list being built between beginEvents() and endEvents().
  int64_t pending_utc_ = 0;
  std::array<uint8_t, cfg::kMatchEvents> pending_{};
  std::array<uint32_t, (cfg::kStoreEvents + 31) / 32> pending_shots_{};
  uint8_t pending_shot_order_ = 0;
  uint8_t pending_count_ = 0, pending_slot_ = kNoIndex;
  bool pending_cut_ : 1 = false;
  bool pending_pool_ : 1 = false;  // an event was lost to the full pool
  bool pending_detail_ : 1 = false;
  // Table being staged between beginTable() and commitTable().
  uint8_t table_competition_ = 0;
  bool table_cut_ = false;
  uint16_t match_count_ = 0, row_count_ = 0, staged_rows_ = 0;
};

// Match/team references can address every configured slot; local detail,
// event, round-label and table-row indices remain bytes.
static_assert(cfg::kStoreTeams < kNoTeam && cfg::kStoreMatches < kNoMatch);
static_assert(cfg::kStoreDetailMatches < kNoIndex &&
              cfg::kStoreEvents < kNoIndex &&
              cfg::kStoreRoundLabels < kNoIndex &&
              cfg::kStoreTableRows < kNoIndex);

// A provider id from settings text: decimal digits only, 1…4,294,967,295
// like the JSON mapper (OQ-298), else 0.
uint32_t decimalId(std::string_view text);
// Phase and minute from the kickoff alone for a match without a provider
// phase.
void estimate(StoredMatch& match, int64_t utc);
bool running(Phase phase);

}  // namespace football
