// A complete-season scan retains only round summaries and four own identities.
// The next independently admitted request stores the selected union. No match
// payload, team catalogue or provider document is buffered here.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <string_view>

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
  std::array<uint32_t, cfg::kRetainedOwnFixtures> own{};
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
      if (kickoff && kickoff / 86400 == calendar_data.days[i] / 86400)
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

class Index {
 public:
  Index(uint32_t club, int64_t utc, DateContext dates = {},
        bool calendar = false);
  bool observe(const provider::Fixture&);
  bool observeDay(uint32_t);
  bool observeRound(const provider::CalendarRound&);
  bool select(Selection&, MatchStore* store = nullptr,
              uint8_t competition = 0) const;
  bool own(Selection&) const;

 private:
  struct Round {
    RoundKey key;
    uint16_t count = 0;
    uint32_t first = 0, last = 0, upcoming = 0;
  };
  struct Own {
    uint32_t id = 0, kickoff = 0;
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
  std::array<Own, cfg::kRetainedOwnFixtures> own_{};
  DateContext dates_;
  int64_t utc_;
  uint32_t club_;
  uint8_t count_ = 0;
  bool complete_ = true, rounds_complete_ = true;
  bool use_calendar_ = false;
  bool selectCalendar(Selection&, MatchStore*, uint8_t competition) const;
};
}  // namespace football::retention
