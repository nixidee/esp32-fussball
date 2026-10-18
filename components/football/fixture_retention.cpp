#include "fixture_retention.h"

#include <algorithm>
#include <new>

#include "matchday.h"

namespace football::retention {
Index::Index(uint32_t club, int64_t utc, DateContext dates, bool calendar)
    : Index(std::span<const uint32_t>(&club, 1), utc, dates, calendar) {}
Index::Index(std::span<const uint32_t> clubs, int64_t utc, DateContext dates,
             bool calendar)
    : dates_(dates), utc_(utc), use_calendar_(calendar) {
  std::size_t count = 0;
  for (const auto id : clubs) {
    if (!id || std::any_of(clubs_.begin(), clubs_.begin() + count,
                           [id](const Club& item) { return item.id == id; }))
      continue;
    if (count == clubs_.size()) {
      complete_ = false;
      break;
    }
    clubs_[count++].id = id;
  }
  if (calendar)
    new (&calendar_) Calendar{};
  else
    new (&rounds_) decltype(rounds_){};
}
bool Index::observeDay(uint32_t day) {
  if (!use_calendar_ || !day) {
    complete_ = false;
    return false;
  }
  auto& calendar = calendar_;
  auto at = std::lower_bound(calendar.days.begin(),
                             calendar.days.begin() + calendar.count, day);
  if (at != calendar.days.begin() + calendar.count && *at == day) return true;
  if (calendar.count == calendar.days.size()) {
    complete_ = false;
    return false;
  }
  std::move_backward(at, calendar.days.begin() + calendar.count,
                     calendar.days.begin() + calendar.count + 1);
  *at = day;
  ++calendar.count;
  return true;
}
bool Index::observeRound(const provider::CalendarRound& stage) {
  if (!use_calendar_ || !stage.from || stage.to < stage.from) {
    complete_ = false;
    return false;
  }
  auto& calendar = calendar_;
  if (calendar.round_count == UINT16_MAX) {
    complete_ = false;
    return false;
  }
  ++calendar.round_count;
  if (stage.to < utc_) {
    if (!calendar.rounds[1].to || stage.to > calendar.rounds[1].to) {
      calendar.rounds[0] = calendar.rounds[1];
      calendar.rounds[1] = stage;
    } else if (!calendar.rounds[0].to || stage.to > calendar.rounds[0].to)
      calendar.rounds[0] = stage;
  } else if (!calendar.rounds[2].to || stage.from < calendar.rounds[2].from) {
    calendar.rounds[3] = calendar.rounds[2];
    calendar.rounds[2] = stage;
  } else if (!calendar.rounds[3].to || stage.from < calendar.rounds[3].from)
    calendar.rounds[3] = stage;
  return true;
}
bool Index::observe(const provider::Fixture& fixture) {
  if (!fixture.facts.id || !fixture.source_matches || !fixture.facts.kickoff) {
    complete_ = false;
    return true;
  }
  const auto kickoff = fixture.facts.kickoff;
  for (auto& club : clubs_) {
    if (!club.id) break;
    if (fixture.home.id != club.id && fixture.away.id != club.id) continue;
    auto& own = club.fixtures;
    if (kickoff < utc_) {
      if (!own[0].id || kickoff > own[0].kickoff ||
          (kickoff == own[0].kickoff && fixture.facts.id < own[0].id))
        own[0] = {fixture.facts.id, kickoff};
    } else {
      Own next{fixture.facts.id, kickoff};
      for (std::size_t i = 1; i < own.size(); ++i) {
        if (own[i].id == next.id) break;
        if (!own[i].id || next.kickoff < own[i].kickoff ||
            (next.kickoff == own[i].kickoff && next.id < own[i].id))
          std::swap(own[i], next);
        if (!next.id) break;
      }
    }
  }
  // Explicit team schedules need no round keys. Their only authoritative
  // result is the closest four own identities, consumed through own().
  if (!fixture.facts.round && fixture.round_label.empty()) {
    rounds_complete_ = false;
    return true;
  }
  RoundKey key;
  key.number = fixture.facts.round;
  key.label = fixture.round_label;
  Round* round = nullptr;
  for (uint8_t i = 0; i < count_; ++i)
    if (rounds_[i].key == key) round = &rounds_[i];
  if (!round) {
    if (count_ == rounds_.size()) {
      complete_ = false;
      return false;
    }
    round = &rounds_[count_++];
    round->key = key;
  }
  if (round->count == UINT16_MAX) {
    complete_ = false;
    return false;
  }
  ++round->count;
  round->first = round->first ? std::min(round->first, kickoff) : kickoff;
  round->last = std::max(round->last, kickoff);
  if (kickoff >= utc_ && (!round->upcoming || kickoff < round->upcoming))
    round->upcoming = kickoff;
  return true;
}
bool Index::own(Selection& out) const {
  if (!complete_ || utc_ <= 0) return false;
  out.own = {};
  auto next = out.own.begin();
  for (const auto& club : clubs_) {
    if (!club.id) break;
    for (const auto& match : club.fixtures)
      if (match.id && std::find(out.own.begin(), next, match.id) == next)
        *next++ = match.id;
  }
  return true;
}
bool Index::select(Selection& out, MatchStore* store,
                   uint8_t competition) const {
  if (use_calendar_) return selectCalendar(out, store, competition);
  if (!complete_ || !rounds_complete_ || utc_ <= 0) return false;
  out.clear();
  if (!own(out)) return false;
  if (!count_) return true;  // an explicitly successful empty season
  int previous = -1, current = -1, next = -1, held = -1, upcoming = -1,
      latest = -1;
  const auto earlier = [&](int a, int b) {
    if (b < 0) return true;
    const auto& one = rounds_[a];
    const auto& two = rounds_[b];
    if (one.first != two.first) return one.first < two.first;
    if (one.key.number != two.key.number)
      return one.key.number < two.key.number;
    return std::string_view(one.key.label.data) <
           std::string_view(two.key.label.data);
  };
  for (uint8_t i = 0; i < count_; ++i) {
    // The current round retains its results through the following local day,
    // matching the existing screen hold, independently of kickoff ordering.
    const auto& round = rounds_[i];
    if (latest < 0 || earlier(latest, i)) latest = i;
    if (round.last <= utc_ &&
        civilDay(utc_, dates_) < civilDay(round.last, dates_) + 2 &&
        (held < 0 || earlier(held, i)))
      held = i;
    if (round.upcoming &&
        (upcoming < 0 || round.upcoming < rounds_[upcoming].upcoming ||
         (round.upcoming == rounds_[upcoming].upcoming &&
          earlier(i, upcoming))))
      upcoming = i;
  }
  current = held >= 0 ? held : upcoming >= 0 ? upcoming : latest;
  for (uint8_t i = 0; i < count_; ++i) {
    if (i == current) continue;
    if (earlier(i, current) && (previous < 0 || earlier(previous, i)))
      previous = i;
    if (earlier(current, i) && earlier(i, next)) next = i;
  }
  for (const int index : {previous, current, next}) {
    if (index < 0) continue;
    const auto at = out.count++;
    out.rounds[at] = rounds_[index].key;
    out.expected[at] = rounds_[index].count;
    if (index == current) out.current = static_cast<int8_t>(at);
  }
  return true;
}
bool Index::selectCalendar(Selection& out, MatchStore* store,
                           uint8_t competition) const {
  if (!complete_ || utc_ <= 0) return false;
  out.clear(true);
  const auto& calendar = calendar_;
  if (calendar.round_count <= 1) {
    bool complete = false;
    out.count = static_cast<uint8_t>(
        matchday::retainedDays(calendar.days.data(), calendar.count, utc_,
                               out.calendar_data.days.data(),
                               out.calendar_data.days.size(), &complete));
    return complete;
  }
  // Beyond the season's final stage, the last stage remains current and its
  // predecessor stays available. Before it, retain past/current/next.
  const unsigned first = calendar.rounds[2].to ? 1 : 0;
  for (unsigned at = first; at < calendar.rounds.size(); ++at) {
    const auto& stage = calendar.rounds[at];
    if (!stage.to) continue;
    auto& selected = out.calendar_data.stages[out.calendar_data.stage_count++];
    selected.from = stage.from;
    selected.to = stage.to;
    selected.number = stage.number;
    if (store && competition) {
      selected.label = store->label(stage.label.data);
      store->roundResult(competition, stage.number, selected.label, 0, false);
    }
  }
  for (uint16_t i = 0; i < calendar.count; ++i) {
    const auto day = calendar.days[i];
    bool keep = false;
    for (uint8_t r = 0; r < out.calendar_data.stage_count; ++r) {
      const auto& range = out.calendar_data.stages[r];
      keep |= day >= range.from && day <= range.to;
    }
    if (keep) {
      if (out.count == out.calendar_data.days.size()) return false;
      out.calendar_data.days[out.count++] = day;
    }
  }
  return true;
}
}  // namespace football::retention
