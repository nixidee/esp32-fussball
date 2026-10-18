#include "store_mappers.h"

namespace football {
namespace {
TeamIndex team(const provider::TeamInput& input, MatchStore& store,
               const MapContext& context) {
  return store.team(context.competition, context.provider, input.id,
                    input.name.data, input.short_name.data);
}
Side actor(uint32_t team, uint32_t home, uint32_t away) {
  return !team          ? Side::kUnknown
         : team == home ? Side::kHome
         : team == away ? Side::kAway
                        : Side::kUnknown;
}
}  // namespace

bool storeMatch(const provider::Fixture& input,
                std::span<const provider::Event> events, MatchStore& store,
                const MapContext& context) {
  using namespace provider;
  if (!input.facts.id) return false;
  // A missing/invalid field cannot erase a fact already known from the source.
  // Local estimated phase/minute may still be refreshed by estimateAll().
  const auto* previous =
      store.find(context.competition, context.provider, input.facts.id);
  StoredMatch match = previous ? *previous : StoredMatch{};
  match.id = input.facts.id;
  match.competition = context.competition;
  match.provider = context.provider;
  // Admit every provided team identity before publishing any score/timing.
  // A corrected opponent that cannot fit must not inherit the previous
  // opponent's label with a new score. The previous match remains intact.
  if (input.home.id) {
    const auto home = team(input.home, store, context);
    if (home == kNoTeam) return false;
    match.home = home;
  }
  if (input.away.id) {
    const auto away = team(input.away, store, context);
    if (away == kNoTeam) return false;
    match.away = away;
  }
  if (input.fields & kKickoff) match.kickoff = input.facts.kickoff;
  if (input.fields & kPhase) {
    match.phase = input.facts.phase;
    match.estimated &= ~kEstimatedPhase;
  }
  if (input.fields & kMinute) {
    match.minute = input.facts.minute;
    match.estimated &= ~kEstimatedMinute;
  }
  if (input.fields & kAdded) match.added = input.facts.added;
  if (input.fields & kPeriod) match.period = input.facts.period;
  if (input.fields & kPrincipal) match.principal = input.facts.principal;
  if (input.fields & kShootout) match.shootout = input.facts.shootout;
  if (input.fields & kRound) match.round = input.facts.round;
  if (!input.round_label.empty())
    match.label = store.label(input.round_label.data);
  if (context.round) match.round = context.round;
  if (context.round_label != kNoIndex) match.label = context.round_label;
  if (input.home.id || input.away.id) {
    match.flags &= ~kMatchOwn;
    if (context.club &&
        (context.club == input.home.id || context.club == input.away.id))
      match.flags |= kMatchOwn;
  }
  if (!(input.fields & kPhase)) estimate(match, context.clock.utc);
  const bool stage = !match.isOwn() && context.observe_events;
  auto* kept = store.upsert(match, context.clock);
  if (context.observe_match && kept)
    context.observe_match(*kept, context.match_context);
  const bool opened = input.carries_events && kept && !stage &&
                      store.beginEvents(*kept, false, context.clock.utc);
  if (opened || context.observe_events) {
    for (const auto& input_event : events) {
      auto event = input_event.facts;
      if (input_event.reported_team)
        event.actor =
            actor(input_event.reported_team, input.home.id, input.away.id);
      if (opened) store.addEvent(event);
      if (context.observe_events)
        context.observe_events(event, context.events_context);
    }
    if (opened) store.endEvents(!input.events_truncated);
  }
  if (kept && input.events_truncated) {
    kept->flags |= kMatchEventsTruncated | kMatchEventsPoolCut;
    store.flag(context.competition, kNoticeEvents, true);
  }
  return kept != nullptr;
}

bool storeTableRow(const provider::TableRow& input, MatchStore& store,
                   const MapContext& context) {
  if (!input.team.id) return false;
  auto row = input.facts;
  row.team = team(input.team, store, context);
  if (row.team == kNoTeam) return false;
  row.group = input.group;
  if (context.club && context.club == input.team.id) row.flags |= kRowOwn;
  return store.appendRow(row);
}
void storeSummaryEvent(const provider::Event& input, MatchStore& store,
                       const StoredMatch& match) {
  auto event = input.facts;
  const auto home =
      match.home < cfg::kStoreTeams ? store.teamAt(match.home).id : 0;
  const auto away =
      match.away < cfg::kStoreTeams ? store.teamAt(match.away).id : 0;
  event.actor = actor(input.reported_team, home, away);
  store.addEvent(event);
}
}  // namespace football
