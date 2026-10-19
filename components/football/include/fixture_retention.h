// A complete-season scan retains round summaries and the union of each
// configured club's previous and next three fixtures, with unique match IDs.
// The next independently admitted request stores the selected union. No match
// payload, team catalogue or provider document is buffered here.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <span>
#include <string_view>

#include "civil_time.h"
#include "provider_adapter.h"

namespace football::retention {
struct RoundKey {
  Text<31> label;
  uint8_t number = 0;
  bool operator==(const RoundKey&) const = default;
};
struct Selection {
  struct Stage {
    uint32_t from = 0, to = 0;
    uint8_t label = kNoIndex, number = 0;
  };
  struct Calendar {
    std::array<uint32_t, cfg::kFixtureCalendarDays> days{};
    std::array<Stage, cfg::kRetainedRounds> stages{};
    uint8_t stage_count = 0;
  };
  union {
    std::array<RoundKey, cfg::kRetainedRounds> rounds;
    Calendar calendar_data;
  };
  std::array<uint32_t, cfg::kRetainedClubFixtures> own{};
  std::array<uint16_t, cfg::kRetainedRounds> expected{};
  uint8_t count = 0;
  bool calendar = false;
  int8_t current = -1;
  Selection() : rounds{} {}
  void clear(bool use_calendar = false) {
    this->~Selection();
    new (this) Selection;
    calendar = use_calendar;
    if (calendar) new (&calendar_data) Calendar{};
  }
  bool contains(uint8_t round, std::string_view label, uint32_t kickoff,
                uint32_t match) const {
    if (match && std::find(own.begin(), own.end(), match) != own.end())
      return true;
    if (!calendar) return roundIndex(round, label) >= 0;
    for (uint8_t i = 0; i < count; ++i)
      if (kickoff && kickoff / timekeeping::kSecondsPerDay ==
                         calendar_data.days[i] / timekeeping::kSecondsPerDay)
        return true;
    return false;
  }
  int roundIndex(uint8_t round, std::string_view label) const {
    if (calendar) return -1;
    for (uint8_t i = 0; i < count; ++i)
      if (rounds[i].number == round &&
          std::string_view(rounds[i].label.data) == label)
        return i;
    return -1;
  }
};

enum class OwnMode : uint8_t { kReplace, kAppend };

struct OwnFixture {
  uint32_t id = 0, kickoff = 0;
};
using OwnFixtures = std::array<OwnFixture, cfg::kRetainedOwnFixtures>;

class Index {
 public:
  Index(uint32_t club, int64_t utc, DateContext dates = {},
        bool calendar = false);
  Index(std::span<const uint32_t> clubs, int64_t utc, DateContext dates = {},
        bool calendar = false);
  bool observe(const provider::Fixture&);
  bool observeDay(uint32_t);
  bool observeRound(const provider::CalendarRound&);
  bool select(Selection&, MatchStore* store = nullptr,
              SourceSlot source = kNoSource) const;
  bool own(Selection&, OwnMode = OwnMode::kReplace) const;
  bool owns(uint32_t match) const;
  // Combine bounded candidates from earlier responses through the same policy.
  void seedOwn(uint32_t club, const OwnFixtures&);
  bool ownCandidates(uint32_t club, OwnFixtures&) const;

 private:
  struct Round {
    RoundKey key;
    uint16_t count = 0;
    uint32_t first = 0, last = 0, upcoming = 0;
  };
  struct Club {
    uint32_t id = 0;
    OwnFixtures fixtures{};
  };
  struct Calendar {
    std::array<uint32_t, cfg::kProviderCalendarDays> days{};
    // Two most recent past stages and two first not-yet-passed stages.
    // The season-round array still determines the union's allocation size.
    std::array<provider::CalendarRound, cfg::kRetainedRounds + 1> rounds{};
    uint16_t count = 0, round_count = 0;
  };
  union {
    std::array<Round, cfg::kStoreRounds> rounds_;
    Calendar calendar_;
  };
  std::array<Club, cfg::kCompetitionMax> clubs_{};
  DateContext dates_;
  int64_t utc_;
  uint8_t count_ = 0;
  bool complete_ = true, rounds_complete_ = true;
  bool use_calendar_ = false;
  void retainOwn(OwnFixtures&, OwnFixture) const;
  bool selectCalendar(Selection&, MatchStore*, SourceSlot source) const;
};
}  // namespace football::retention
