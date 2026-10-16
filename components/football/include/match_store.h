// Compact football data store (OQ-227): the matches of every competition,
// all league tables, a team directory and the events of the own club's
// matches. Rows hold numeric provider ids and one-byte directory indices
// instead of names; full team names are kept only for own matches. Which
// matchdays are stored is up to the fetches; expire() drops past matchdays.
// Plain fixed storage: no heap, no locking (the owner serialises access).
// Pure, so the native tests compile it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "app_config.h"
#include "football_model.h"

namespace football {

// Unknown index (team, round label, own-match slot) and unknown value
// (goals, minute, table count).
inline constexpr uint8_t kNoIndex = 0xff;
inline constexpr uint8_t kNoValue = 0xff;
// Unknown signed table value (goal difference, points).
inline constexpr int16_t kNoNumber = INT16_MIN;

struct Goals {
  uint8_t home = kNoValue, away = kNoValue;
  bool valid() const { return home != kNoValue && away != kNoValue; }
  bool operator==(const Goals&) const = default;
};

// Bits of StoredMatch::flags.
inline constexpr uint8_t kMatchOwn = 0x01;
inline constexpr uint8_t kMatchEventsTruncated = 0x02;
// Events of the own match were lost to the full event pool (kNoticeEvents
// stays until no own match of the competition carries it, OQ-311).
inline constexpr uint8_t kMatchEventsPoolCut = 0x04;
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
  uint8_t competition = 0;     // stable competition id (1-255)
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  uint8_t home = kNoIndex, away = kNoIndex;  // team directory
  uint8_t round = 0;           // provider round number, 0 = none
  uint8_t label = kNoIndex;    // round label pool
  Phase phase = Phase::kUnknown;
  uint8_t minute = kNoValue;
  Goals principal, halftime, regulation, extra_time, shootout;
  uint8_t flags = 0, estimated = 0;
  uint8_t own = kNoIndex;      // own-match slot
  bool isOwn() const { return (flags & kMatchOwn) != 0; }
};

struct StoredTeam {
  uint32_t id = 0;  // provider team id, 0 = free entry
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  Text<26> short_name;
};

// Bits of StoredRow::flags.
inline constexpr uint8_t kRowOwn = 0x01;
inline constexpr uint8_t kRowPositionEstimated = 0x02;  // list order

struct StoredRow {
  uint8_t competition = 0, team = kNoIndex;
  uint8_t position = kNoValue, played = kNoValue, won = kNoValue,
          drawn = kNoValue, lost = kNoValue, goals_for = kNoValue,
          goals_against = kNoValue, flags = 0;
  int16_t difference = kNoNumber, points = kNoNumber;
};

// Team of an event relative to its match.
enum class Side : uint8_t { kUnknown, kHome, kAway };

// Bits of StoredEvent::flags.
inline constexpr uint8_t kEventRemoved = 0x01;  // withdrawn by the provider
inline constexpr uint8_t kEventShootout = 0x02;
// From the detail request (ESPN match summary: substitutions); the detail
// and the list events each replace only their own events.
inline constexpr uint8_t kEventDetail = 0x08;

struct StoredEvent {
  uint32_t identity = 0, revision = 0;
  uint8_t slot = kNoIndex;  // own-match slot holding it, kNoIndex = free
  EventType type = EventType::kGoal;
  uint8_t minute = kNoValue, added = kNoValue;
  Side actor = Side::kUnknown, beneficiary = Side::kUnknown;
  Goals after;
  uint8_t flags = 0;
  // Scorer or booked player; for a substitution the player going off, or
  // both names when the provider does not say who comes on.
  Text<32> player;
  Text<32> player_in;  // substitution: the player coming on
};

// An own-club match: full team names and its events in order.
struct OwnMatch {
  uint32_t match = 0;  // match id, 0 = free slot
  uint8_t competition = 0;
  uint8_t event_count = 0;
  std::array<uint8_t, cfg::kMatchEvents> events{};  // event pool indices
  Text<cfg::kNameBytes> home_name, away_name;
};

// Bits of the per-competition notices: data the store could not hold
// (cause E19 store_full, OQ-310). Each kind is cleared when it is next
// stored completely (OQ-311): matches and their teams by a complete
// fixtures run (beginRun()/settleRun(), a live run only adds), the table and
// its teams by a replaced table, events when no own match of the
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

class MatchStore {
 public:
  // Out of line on purpose: an implicit constexpr constructor would place a
  // static store in initialised data, copied from flash at boot (22 KB).
  MatchStore();
  void clear();

  // Directory index of a team, added or with its short name updated;
  // kNoIndex for id 0 or a full directory (also notes kNoticeTableTeams for
  // 'competition' while its table is staged, else kNoticeMatchTeams).
  uint8_t team(uint8_t competition, cfg::Provider provider, uint32_t id,
               std::string_view short_name);
  // Pool index of a round label; kNoIndex for an empty text or a full pool.
  uint8_t label(std::string_view text);

  // Stores 'incoming' (teams and label already interned), replacing the
  // stored match with the same competition, provider and id. Kept from the
  // stored match: the time of the last score change (set to 'clock.mono_s'
  // when both scores are known and differ) and the final-whistle time. A
  // match first seen finished gets 'clock.utc' as estimated final whistle,
  // or the expected end when it was never seen running. An own match gets
  // an own-match slot with the full names; a full slot table gives the
  // slot of the own match farthest from now when this one is nearer. A
  // full store makes room by removing the other club's match farthest from
  // now when this one is own or nearer; nullptr (kNoticeMatches) when it
  // does not fit. The list stays sorted by kickoff.
  StoredMatch* upsert(const StoredMatch& incoming, const Clock& clock,
                      std::string_view home_name = {},
                      std::string_view away_name = {});
  // Removes the matchdays of 'competition' that have passed
  // (matchday::passed(): kept until the end of the local day after its last
  // kickoff, OQ-202).
  // 'local_offset_s' is the local UTC offset at 'utc'. Matches without a
  // kickoff stay.
  void expire(uint8_t competition, int64_t utc, int32_t local_offset_s);
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
  // rows unless a row did not fit the buffer, no row was staged or the new
  // table does not fit the row pool; otherwise the stored table stays. A row
  // that does not fit the buffer and a table that does not fit the pool note
  // kNoticeTable; a replaced table clears it. appendRow() is false when the
  // row was not staged.
  void beginTable(uint8_t competition);
  bool appendRow(const StoredRow& row);
  bool commitTable(bool complete);
  std::size_t stagedRows() const { return staged_rows_; }

  // Replaces the events of own match 'match' in three steps: begin, add in
  // order, end. When neither the previous nor the new list was cut, a
  // previous event whose identity is missing from the new list stays as a
  // removal marker (revision + 1) after the new ones. More than
  // kMatchEvents events keep the latest ones; a full pool drops the rest.
  // Both set kMatchEventsTruncated. endEvents(false), for a response that
  // broke off, keeps every previous event and adds the new ones after them.
  // 'detail' events come from the detail request (kEventDetail): the detail
  // and the list events each replace only their own events, the other
  // origin's stay ahead of the new list; when the slot is full detail
  // events give way first. A full pool gives this match the oldest event of
  // the own match farthest from 'utc' when this one is nearer (OQ-292, the
  // rule of the own-match slots); that match is marked truncated and its
  // competition notes kNoticeEvents. 'utc' 0 = unknown, nothing is taken.
  // Events lost to the full pool mark the match kMatchEventsPoolCut until
  // its next complete list keeps every event (endEvents(true)).
  // False if 'match' has no slot.
  bool beginEvents(const StoredMatch& match, bool detail = false,
                   int64_t utc = 0);
  void addEvent(const StoredEvent& event);
  void endEvents(bool complete = true);
  // Appends a saved event (competition file) to the list of own match
  // 'match' as it was stored, flags included; false if 'match' has no
  // slot, its list is full or the pool is (kNoticeEvents).
  bool restoreEvent(const StoredMatch& match, const StoredEvent& event);

  std::size_t matchCount() const { return match_count_; }
  const StoredMatch& match(std::size_t i) const { return matches_[i]; }
  const StoredMatch* find(uint8_t competition, cfg::Provider provider,
                          uint32_t id) const;
  const StoredTeam& teamAt(uint8_t i) const { return teams_[i]; }
  const Text<31>& labelAt(uint8_t i) const { return labels_[i]; }
  std::size_t rowCount() const { return row_count_; }
  const StoredRow& row(std::size_t i) const { return rows_[i]; }
  const OwnMatch& ownAt(uint8_t i) const { return own_[i]; }
  const StoredEvent& eventAt(uint8_t i) const { return events_[i]; }
  uint8_t notices(uint8_t competition) const;
  // True when a match of 'competition' kicked off more than
  // cfg::kUnmaintainedAfterS before 'utc' and is neither over (finished,
  // postponed, cancelled, abandoned) nor has a result: its provider does not
  // maintain the competition (OQ-281). Asked before expire() drops it.
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
  int matchIndex(uint8_t competition, cfg::Provider provider,
                 uint32_t id) const;
  void removeMatch(std::size_t i);
  void freeSlot(uint8_t slot);
  uint8_t ownSlot(const StoredMatch& match, int64_t utc);
  void note(uint8_t competition, uint8_t bits);
  std::size_t sortedPosition(const StoredMatch& match) const;
  void dropRows(uint8_t competition);
  int takeEvent();
  StoredMatch* ownRow(uint8_t slot);
  void settleEvents(uint8_t competition);
  StoredMatch* place(StoredMatch m, std::size_t from);

  std::array<StoredMatch, cfg::kStoreMatches> matches_{};
  std::array<StoredTeam, cfg::kStoreTeams> teams_{};
  std::array<StoredRow, cfg::kStoreTableRows> rows_{};
  std::array<StoredRow, cfg::kStoreTableStageRows> stage_{};
  std::array<OwnMatch, cfg::kStoreOwnMatches> own_{};
  std::array<StoredEvent, cfg::kStoreEvents> events_{};
  std::array<Text<31>, cfg::kStoreRoundLabels> labels_{};
  struct Notice {
    // 'noted': bits noted since beginRun().
    uint8_t competition = 0, bits = 0, noted = 0;
  };
  std::array<Notice, cfg::kCompetitionMax> notices_{};
  // Event list being built between beginEvents() and endEvents().
  int64_t pending_utc_ = 0;
  std::array<uint8_t, cfg::kMatchEvents> pending_{};
  uint8_t pending_count_ = 0, pending_slot_ = kNoIndex;
  bool pending_cut_ : 1 = false;
  bool pending_pool_ : 1 = false;  // an event was lost to the full pool
  bool pending_detail_ : 1 = false;
  // Table being staged between beginTable() and commitTable().
  uint8_t table_competition_ = 0;
  bool table_cut_ = false;
  uint16_t match_count_ = 0, row_count_ = 0, staged_rows_ = 0;
};

// Store indices are one byte (views, file headers and event lists).
static_assert(cfg::kStoreTeams < kNoIndex && cfg::kStoreOwnMatches < kNoIndex &&
              cfg::kStoreEvents < kNoIndex &&
              cfg::kStoreRoundLabels < kNoIndex &&
              cfg::kStoreMatches < kNoIndex && cfg::kStoreTableRows < kNoIndex);

// A provider id from settings text: decimal digits only, 1…4,294,967,295
// like the JSON mapper (OQ-298), else 0.
uint32_t decimalId(std::string_view text);
// Phase and minute from the kickoff alone for a match without a provider
// phase.
void estimate(StoredMatch& match, int64_t utc);
bool running(Phase phase);

}  // namespace football
