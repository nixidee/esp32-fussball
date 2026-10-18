// Pure phase and content selection for the common screen policy. Inputs are
// references to the existing canonical models; no retained snapshot or heap.
// The caller serialises store/settings access and resolves display eligibility
// separately through settings::screen_policy::resolve().
#pragma once

#include <array>
#include <cstdint>

#include "match_store.h"
#include "screen_policy.h"
#include "settings_model.h"

namespace football::selection {

using ScreenPhase = settings::screen_policy::Phase;

struct MatchKey {
  uint32_t id = 0;
  uint8_t competition = 0;
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  bool valid() const { return id != 0 && competition != 0; }
  bool operator==(const MatchKey&) const = default;
};

struct Input {
  settings::screen_policy::SelectionRules rules;
  const settings::CompetitionRoutes& competitions;
  const MatchStore& store;
  Clock clock;
  DateContext dates;
  // Supplied only by a successful, complete and fresh coverage proof. This
  // selector never infers an empty schedule from absent display-store rows.
  bool break_proven = false;
  Input(settings::screen_policy::SelectionRules values,
        const settings::CompetitionRoutes& entries, const MatchStore& matches,
        Clock time, DateContext calendar = {}, bool proven = false)
      : rules(values),
        competitions(entries),
        store(matches),
        clock(time),
        dates(calendar),
        break_proven(proven) {}
  Input(const settings::screen_policy::Options& policy,
        const settings::CompetitionRoutes& entries, const MatchStore& matches,
        Clock time, DateContext calendar = {}, bool proven = false)
      : Input(settings::screen_policy::selectionRules(policy, entries), entries,
              matches, time, calendar, proven) {}
};

struct Selection {
  ScreenPhase phase = ScreenPhase::kIdle;
  MatchKey focus;  // only the focused special own match, otherwise empty
};

// A match's bounded applicable pre/live/post interval at the supplied clock.
// An unfinished match has no fabricated post interval after its hard bound.
// reported_running includes an already justified explicit interruption;
// estimates remain identifiable through the canonical match's estimated bits.
struct Window {
  int64_t begin = 0, end = 0;
  bool active = false;
  bool reported_running = false;
  bool own_special = false;
};

enum class Content : uint8_t {
  kEmpty,
  kBefore,
  kLive,
  kAfter,
  kResult,
  kPreview
};
struct Single {
  MatchKey match;
  Content content = Content::kEmpty;
};

// A local-date cluster chooses a round; it does not limit a verified round's
// screen content. first_day/last_day are civil dates counted from 1970-01-01.
// For fallback groups those same dates are the explicit content boundary.
struct Round {
  MatchKey anchor;
  int32_t first_day = 0, last_day = 0;
  bool verified = false;
  bool valid() const { return anchor.valid(); }
};

const StoredMatch* find(const Input& input, const MatchKey& match);
bool ownSpecial(const Input& input,
                const settings::CompetitionRoute& competition);
Window window(const Input& input, const settings::CompetitionRoute& competition,
              const StoredMatch& match);
// One highest phase: Own > OtherGames > Matchday > Idle/optional Break.
// Confirmed running own games beat every own buffer, then competition order.
// A fetch error is deliberately absent: retained data keep their time bounds.
Selection select(const Input& input);
// Own phase has only the focused Single. OtherGames prefers a unique reported
// running game, else a unique window candidate. Static result/preview requires
// a selected club; no unique active candidate never invents a foreign preview.
Single single(const Input& input, const Selection& selected,
              const settings::CompetitionRoute& competition);
// Current local-date phase period, empty outside every eligible cluster.
Round phasePeriod(const Input& input,
                  const settings::CompetitionRoute& competition);
// Selected Matchday content. Current: active cluster, then held, then next.
// Next: active or next, never a held-only group. A verified round's content
// includes all its known fixtures, even outside this chosen date cluster.
Round round(const Input& input, const settings::CompetitionRoute& competition,
            cfg::MatchdayMode mode);
// Complete held/current/next navigation, using the same round identity and
// date-cluster rules as the automatic selection. Split phase clusters of one
// verified sporting round appear only once, with all their fixtures.
struct RoundReach {
  std::array<Round, 3> round{};
  uint8_t count = 0;
  int8_t selected = -1;
};
RoundReach rounds(const Input& input,
                  const settings::CompetitionRoute& competition,
                  cfg::MatchdayMode mode);
bool roundContains(const Input& input, const Round& round,
                   const StoredMatch& match);

static_assert(sizeof(MatchKey) <= 8 && sizeof(Selection) <= 16 &&
              sizeof(Single) <= 16 && sizeof(Round) <= 24);

}  // namespace football::selection
