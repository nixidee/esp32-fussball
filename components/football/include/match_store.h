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
// Bits of StoredMatch::estimated: the value was derived by the device, not
// reported by the provider.
inline constexpr uint8_t kEstimatedPhase = 0x01;
inline constexpr uint8_t kEstimatedMinute = 0x02;
inline constexpr uint8_t kEstimatedFinish = 0x04;
// Bits of StoredMatch::fallback: the value comes from the competition's
// fallback source, not from its primary provider.
inline constexpr uint8_t kFallbackKickoff = 0x01;
inline constexpr uint8_t kFallbackPhase = 0x02;
inline constexpr uint8_t kFallbackMinute = 0x04;
inline constexpr uint8_t kFallbackPrincipal = 0x08;
inline constexpr uint8_t kFallbackHalftime = 0x10;
inline constexpr uint8_t kFallbackRegulation = 0x20;
inline constexpr uint8_t kFallbackExtraTime = 0x40;
inline constexpr uint8_t kFallbackShootout = 0x80;

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
  uint8_t flags = 0, estimated = 0, fallback = 0;
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
inline constexpr uint8_t kEventFallback = 0x04;  // from the fallback source
// From the primary's detail request (ESPN match summary: substitutions).
inline constexpr uint8_t kEventDetail = 0x08;
// The bits naming where an event came from; each origin replaces only its
// own events.
inline constexpr uint8_t kEventOrigin = kEventFallback | kEventDetail;

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

// One matchday of a competition: kickoffs less than kMatchdayGapHours apart.
struct Matchday {
  uint32_t first = 0, last = 0;  // first and last kickoff (UTC seconds)
  uint8_t count = 0;
};

// Bits of the per-competition notices.
inline constexpr uint8_t kNoticeMatches = 0x01;  // a match did not fit
inline constexpr uint8_t kNoticeTable = 0x02;    // a table row did not fit
inline constexpr uint8_t kNoticeTeams = 0x04;    // team directory full
inline constexpr uint8_t kNoticeEvents = 0x08;   // an event did not fit
// The primary source's last match request failed: the fallback source's
// matches are stored on their own and stand in for the primary's.
inline constexpr uint8_t kNoticeFallbackRoute = 0x10;
// The fallback source's last request failed.
inline constexpr uint8_t kNoticeFallbackFailed = 0x20;

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
  // kNoIndex for id 0 or a full directory (also notes kNoticeTeams for
  // 'competition').
  uint8_t team(uint8_t competition, cfg::Provider provider, uint32_t id,
               std::string_view short_name);
  // Updates the short name of a team the directory holds (core data adds no
  // teams); false when it holds none with this provider and id.
  bool rename(cfg::Provider provider, uint32_t id, std::string_view short_name);
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
  // does not fit. A value the stored match took from its fallback source
  // stays, with its fallback bit, while 'incoming' lacks it (or only
  // estimated it). The list stays sorted by kickoff.
  StoredMatch* upsert(const StoredMatch& incoming, const Clock& clock,
                      std::string_view home_name = {},
                      std::string_view away_name = {});
  // Fills the stored match 'id' of 'provider' in the competition of
  // 'secondary' from its paired fallback match 'secondary' (mapped from the
  // fallback source): kickoff, phase, minute and the five scores the stored
  // match lacks, only estimated or took from the fallback before take the
  // fallback's reported values and their fallback bits; estimated fallback
  // values fill nothing. 'swapped' swaps home and away of every score. A
  // filled final whistle state gets the estimated final-whistle time as in
  // upsert(). Returns the stored match, nullptr when it is not stored.
  StoredMatch* fill(const StoredMatch& secondary, cfg::Provider provider,
                    uint32_t id, bool swapped, const Clock& clock);
  // Removes the matches of 'provider' in 'competition' (the fallback
  // route's matches once the primary source answers again).
  void removeProvider(uint8_t competition, cfg::Provider provider);
  // Removes the matchdays of 'competition' that have passed: a matchday is
  // kept until the end of the local day after its last kickoff (OQ-202).
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
  // commit. The rows are staged next to the stored ones; when the staging
  // does not fit, the stored rows of the competition make room first.
  // commitTable(true) replaces the table with the staged rows unless a row
  // did not fit or no row was staged; otherwise the staged rows are
  // dropped. A row that does not fit and a table lost to making room note
  // kNoticeTable. appendRow() is false when the row was not staged.
  void beginTable(uint8_t competition);
  bool appendRow(const StoredRow& row);
  bool commitTable(bool complete);
  std::size_t stagedRows() const { return staged_rows_; }
  // Whether the stored table of 'competition' has a row of a 'provider'
  // team.
  bool hasTable(uint8_t competition, cfg::Provider provider) const;

  // Replaces the events of own match 'match' in three steps: begin, add in
  // order, end. When neither the previous nor the new list was cut, a
  // previous event whose identity is missing from the new list stays as a
  // removal marker (revision + 1) after the new ones. More than
  // kMatchEvents events keep the latest ones; a full pool drops the rest.
  // Both set kMatchEventsTruncated. endEvents(false), for a response that
  // broke off, keeps every previous event and adds the new ones after them.
  // 'fallback' events come from the fallback source of a paired match
  // (kEventFallback; 'swapped' swaps their sides and scores), 'detail'
  // events from the primary's detail request (kEventDetail): each origin
  // replaces only its own events, the other origins' stay ahead of the new
  // list; when the slot is full fallback events give way first, then detail
  // events. False if 'match' has no slot.
  bool beginEvents(const StoredMatch& match, bool fallback = false,
                   bool swapped = false, bool detail = false);
  void addEvent(const StoredEvent& event);
  void endEvents(bool complete = true);
  // Appends a saved event (competition file) to the list of own match
  // 'match' as it was stored, flags included; false if 'match' has no
  // slot, its list is full or the pool is (kNoticeEvents).
  bool restoreEvent(const StoredMatch& match, const StoredEvent& event);

  // Matchday 'ordinal' (0 = first stored) of 'competition'; false if none.
  bool matchday(uint8_t competition, uint8_t ordinal, Matchday& out) const;

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
  // Sets or clears notice 'bits' of 'competition'.
  void flag(uint8_t competition, uint8_t bits, bool on);
  // Whether event 'e' of own slot 'slot' is shown: the primary source's
  // events win, so a fallback goal is hidden while the slot holds a goal of
  // the primary and a fallback missed penalty while it holds a primary
  // missed penalty (removal markers do not count).
  bool eventShown(uint8_t slot, const StoredEvent& e) const;
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
  StoredMatch* place(StoredMatch m, std::size_t from);

  std::array<StoredMatch, cfg::kStoreMatches> matches_{};
  std::array<StoredTeam, cfg::kStoreTeams> teams_{};
  std::array<StoredRow, cfg::kStoreTableRows> rows_{};
  std::array<OwnMatch, cfg::kStoreOwnMatches> own_{};
  std::array<StoredEvent, cfg::kStoreEvents> events_{};
  std::array<Text<31>, cfg::kStoreRoundLabels> labels_{};
  struct Notice {
    uint8_t competition = 0, bits = 0;
  };
  std::array<Notice, cfg::kCompetitionMax> notices_{};
  // Event list being built between beginEvents() and endEvents().
  std::array<uint8_t, cfg::kMatchEvents> pending_{};
  uint8_t pending_count_ = 0, pending_slot_ = kNoIndex;
  bool pending_cut_ : 1 = false;
  bool pending_fallback_ : 1 = false;
  bool pending_swapped_ : 1 = false;
  bool pending_detail_ : 1 = false;
  // Table being staged between beginTable() and commitTable(); staged rows
  // carry competition 0.
  uint8_t table_competition_ = 0;
  bool table_cut_ = false, table_dropped_ = false;
  uint16_t match_count_ = 0, row_count_ = 0, staged_rows_ = 0;
};

static_assert(cfg::kStoreTeams < kNoIndex && cfg::kStoreOwnMatches < kNoIndex &&
              cfg::kStoreEvents < kNoIndex &&
              cfg::kStoreRoundLabels < kNoIndex);

// A provider id from settings text: decimal digits only (at most nine),
// else 0.
uint32_t decimalId(std::string_view text);
// Phase and minute from the kickoff alone for a match without a provider
// phase.
void estimate(StoredMatch& match, int64_t utc);
bool running(Phase phase);
// True once 'utc' has reached the end of the local day after the local day
// of 'last_kickoff'.
bool matchdayPassed(uint32_t last_kickoff, int64_t utc,
                    int32_t local_offset_s);

}  // namespace football
