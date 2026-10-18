// Real volatile working-directory acceptance; host-only fixtures.
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "match_store.h"
#include "store_mappers.h"
#include "store_views.h"

using namespace football;
constexpr auto kProvider = cfg::Provider::kEspn;
void clipping_and_directory_full_admission() {
  MatchStore source;
  auto index = source.team(1, kProvider, 1, "Twenty-six bytes of complete text",
                           "ABCDE");
  assert(std::strlen(source.teamAt(index).name.data) == cfg::kTeamNameBytes);
  assert(std::string_view(source.teamAt(index).short_name.data) == "ABC");
  source.team(1, kProvider, 1, "New original", "");
  assert(source.teamAt(index).short_name.empty());
  assert(source.team(1, kProvider, 1, "") == index);
  assert(std::string_view(source.teamAt(index).name.data) == "New original");
  const auto away = source.team(1, kProvider, 2, "Other original", "OTH");
  StoredMatch match;
  match.id = 1000;
  match.competition = 1;
  match.provider = kProvider;
  match.home = index;
  match.away = away;
  match.principal = {1, 0};
  assert(source.upsert(match, {100, 1}));
  for (uint32_t id = 3; id <= cfg::kStoreTeams; ++id)
    assert(source.team(1, kProvider, id, "Filler", "FIL") != kNoTeam);
  assert(source.team(1, kProvider, 10000, "Missing", "MIS") == kNoTeam);
  // The approved single 26/3 directory replaces duplicate detail raw names.
  // Overflow is an explicit failed admission, never a new score attached to
  // an unavailable or previous opponent's identity/name.
  provider::Fixture incoming;
  incoming.facts.id = match.id;
  incoming.home.id = 10000;
  incoming.home.name.assign("New unavailable opponent");
  incoming.away.id = 2;
  incoming.away.name.assign("Other original");
  incoming.facts.principal = {9, 8};
  incoming.fields = provider::kPrincipal;
  MapContext context;
  context.competition = 1;
  context.provider = kProvider;
  context.clock = {100, 2};
  assert(!storeMatch(incoming, {}, source, context));
  const auto* stored = source.find(1, kProvider, match.id);
  assert(stored && stored->home == index && stored->away == away &&
         stored->principal == match.principal);
  assert(std::string_view(view::teamName(source, *stored, true)) ==
         "New original");
  assert(std::string_view(view::teamName(source, *stored, false)) ==
         "Other original");
  incoming.facts.id = 1001;
  assert(!storeMatch(incoming, {}, source, context));
  assert(!source.find(1, kProvider, 1001) && source.matchCount() == 1);
  assert(source.notices(1) & kNoticeMatchTeams);
}
int main() {
  clipping_and_directory_full_admission();
  std::cout
      << "working-set groups PASS; clipped provider values, explicit empty and "
         "full-directory admission preserve prior pairing/score\n";
}
