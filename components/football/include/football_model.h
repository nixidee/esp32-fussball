#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

#include "app_config.h"

namespace football {
enum class Source : uint8_t {
  kNone,
  kProvider,
  kEstimated,
  kOpenLigaDb,
  kApiFootball,
  kEspn,
  kFootballData
};
constexpr bool confirmed(Source source) {
  return source == Source::kProvider || source >= Source::kOpenLigaDb;
}
constexpr Source providerSource(cfg::Provider provider) {
  return static_cast<Source>(3 + static_cast<uint8_t>(provider));
}
template <class T>
struct Value {
  T value{};
  Source source = Source::kNone;
  bool valid() const { return source != Source::kNone; }
  bool operator==(const Value&) const = default;
};
template <std::size_t N>
struct Text {
  char data[N + 1]{};
  void assign(std::string_view text) {
    std::size_t count = text.size() < N ? text.size() : N;
    while (count < text.size() && count > 0 &&
           (static_cast<unsigned char>(text[count]) & 0xc0) == 0x80)
      --count;
    std::memset(data, 0, sizeof(data));
    std::memcpy(data, text.data(), count);
  }
  bool empty() const { return data[0] == 0; }
  bool operator==(const Text&) const = default;
};
struct Id : Text<cfg::kIdBytes> {
  // IDs are identities: never accept a truncated prefix as a different ID.
  bool assign(std::string_view value) {
    if (value.size() > cfg::kIdBytes || value.find('\0') != value.npos) {
      data[0] = 0;
      return false;
    }
    Text<cfg::kIdBytes>::assign(value);
    return true;
  }
  bool operator==(const Id&) const = default;
};
struct Team {
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  Id id;
  Text<cfg::kNameBytes> name;
  Text<24> short_name;
};
struct Score {
  Value<int16_t> home, away;
  bool valid() const { return home.valid() && away.valid(); }
  bool operator==(const Score&) const = default;
};
enum class Phase : uint8_t {
  kUnknown,
  kScheduled,
  kLive,
  kHalfTime,
  kExtraTime,
  kPenalties,
  kFinished,
  kPostponed,
  kCancelled,
  kAbandoned
};
enum class EventType : uint8_t {
  kGoal,
  kOwnGoal,
  kPenaltyGoal,
  kPenaltyMissed,
  kYellow,
  kYellowRed,
  kRed,
  kSubstitution,
  kInjury,
  kVar
};
struct MatchEvent {
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  uint32_t identity = 0, revision = 0;
  EventType type = EventType::kGoal;
  Value<int16_t> minute, added;
  Id actor_team, beneficiary;
  Text<32> player, player_in, player_out;
  Text<24> detail;
  Score after;
  bool removed = false;
  bool shootout = false;
  bool operator==(const MatchEvent&) const = default;
};
struct Match {
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  Id id;
  uint8_t route = 0;
  Value<uint16_t> round;
  Text<32> round_label;
  Team home, away;
  Value<int64_t> kickoff, finished_at, provider_changed;
  Phase phase = Phase::kUnknown;
  Source phase_source = Source::kNone;
  Value<int16_t> minute;
  Score principal, halftime, regulation, extra_time, shootout;
  std::array<uint8_t, cfg::kMatchEvents>
      events{};  // indices in snapshot event pool
  uint8_t event_count = 0;
  bool events_truncated = false;
  bool own = false;
  int64_t score_changed_ms = 0, fetched_ms = 0;
};
struct Standing {
  bool own = false;
  Team team;
  Value<int16_t> position, played, won, drawn, lost, goals_for, goals_against,
      difference, points;
};
struct Snapshot {
  // Runtime construction keeps fixed storage in BSS instead of embedding
  // three almost-empty 27 KB initial images in the firmware. No heap.
  Snapshot() noexcept;
  void reset() noexcept;
  std::array<Match, cfg::kMatches> matches{};
  std::array<Standing, cfg::kTableRows> table{};
  std::array<MatchEvent, cfg::kSnapshotEvents> events{};
  uint8_t match_count = 0, table_count = 0, event_count = 0;
  uint32_t generation = 0, revision = 0;
  int64_t fetched_ms = 0;
  bool stale = true, configured = false;
  Text<96> routing_notice;
  Text<96> error;
};
struct Selection {
  Id id, season;
  Text<cfg::kNameBytes> name;
};
struct Capabilities {
  bool live, status, minute, cards, substitutions, table, cups, key;
  // Automatic browser crest import is a retained, deferred enhancement.
  bool crests = false;
  uint32_t minimum_interval_ms = 0;
};
const char* providerName(cfg::Provider provider);
Capabilities capabilities(cfg::Provider provider);
int64_t parseUtc(const char* text);
void estimate(Match& match, int64_t utc);
bool matchWindow(const Match& match, int64_t utc, uint16_t before,
                 uint16_t after);
// The conference remains active between the first and last fixture of the
// configured rounds. Own catch-up/next-round fixtures have separate
// windows.
bool conferenceWindow(const Snapshot& data, int64_t utc, uint16_t before,
                      uint16_t after);
uint32_t fingerprint(std::string_view value);
void demo(Snapshot& out);
}  // namespace football
