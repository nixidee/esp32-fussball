// Private UI composition interface. One controller owns the model, navigation
// and view; a presenter borrows them only during a repository callback.
#pragma once
#include <cstdio>
#include <cstring>
#include <ctime>

#include "football_screen.h"
#include "football_service.h"
#include "image_service.h"
#include "navigation_policy.h"
#include "network_service.h"
#include "visible_names.h"

namespace ui {
struct BindContext {
  int64_t utc, now;
  bool valid, connected, ap;
  const network::Status* net;
  uint32_t status_state = 0;
  bool bound = false, phase_changed = false;
  settings::screen_policy::Phase required_phase =
      settings::screen_policy::Phase::kIdle;
  football::names::Targets names{};
};
struct PresentationState {
  uint32_t pill_state = 0, screen_status = 0;
  int64_t pill_until = 0;
  bool pill_known = false, no_badge = false, card_network = false;
  uint8_t card_fault = 0, on_screen = 0;
  bool starting = true;
};
struct Subject {
  bool network = false;
  uint8_t competition = 0;
};
class Presenter {
 public:
  Presenter(const settings::DisplaySettings& settings, ViewModel& output,
            const settings::screen_policy::Options& rules,
            PresentationState& presentation, std::size_t inputs,
            const football::Published* publication = nullptr,
            const PresentationFacts* facts = nullptr,
            football::names::Targets* names = nullptr,
            const settings::UiConfig* configuration = nullptr,
            const char* version = "")
      : model(settings),
        policy(rules),
        view(output),
        presentation(presentation),
        input_count(inputs),
        published(publication),
        facts(facts),
        name_targets(names),
        configuration(configuration),
        version(version) {}
  // Compose under the repository read. After image selection outside that
  // lock, finishStatus applies the one shared warning fingerprint/timer.
  uint32_t bind(const BindContext& context);
  void finishStatus(uint32_t state, int64_t now,
                    const images::Status& selected_image);
  void bindStart(uint8_t step);

 private:
  const settings::DisplaySettings& model;
  const settings::screen_policy::Options& policy;
  ViewModel& view;
  PresentationState& presentation;
  std::size_t input_count;
  const football::Published* published;
  const PresentationFacts* facts;
  football::names::Targets* name_targets;
  const settings::UiConfig* configuration;
  const char* version;
  template <std::size_t N, std::size_t M>
  void teamTarget(char (&out)[N], char (&short_out)[M],
                  const football::MatchStore& store,
                  football::TeamIndex index) {
    if (index == football::kNoTeam) return;
    const auto& team = store.teamAt(index);
    std::memcpy(short_out, team.short_name.data,
                std::min(M, sizeof(team.short_name.data)));
    if (name_targets)
      name_targets->add({team.id, team.provider, out, N, short_out, M});
  }
  void setStyle(screen::Type type);
  const char* translated(const char* de, const char* en) const {
    return model.language ? en : de;
  }
  template <std::size_t N>
  void prefix(char (&out)[N + 1], const char* text) {
    football::Text<N> value;
    value.assign(text);
    std::memcpy(out, value.data, sizeof(out));
  }
  template <std::size_t N>
  void namePrefix(char (&out)[N + 1], const char* text) {
    football::Text<(N < cfg::kTeamNameBytes ? N : cfg::kTeamNameBytes)> value;
    value.assign(text);
    std::memset(out, 0, sizeof(out));
    std::memcpy(out, value.data, sizeof(value.data));
  }
  template <std::size_t N>
  void pair(char (&out)[N], const char* format, const football::Goals& score) {
    char text[24];
    std::snprintf(text, sizeof(text), format, score.home, score.away);
    prefix<N - 1>(out, text);
  }
  bool started(const football::StoredMatch& match) const {
    return football::view::started(match.phase) ||
           (match.phase == football::Phase::kSuspended &&
            match.first_live_observed_utc != 0);
  }
  void value(char* out, std::size_t size, int n, bool known);
  void value(char* out, std::size_t size, uint8_t n);
  void value(char* out, std::size_t size, int16_t n);
  bool localCivil(int64_t utc, std::tm& out) const;
  void localDate(char* out, std::size_t size, int64_t utc);
  void localTime(char* out, std::size_t size, int64_t utc);
  void calendarTime(char* out, std::size_t size, uint8_t instant);
  const settings::CompetitionRoute* entryOf(uint8_t id) const;
  void roundCaption(const football::MatchStore& store,
                    const football::StoredMatch& match, bool date);
  void cardText(const char* title, const char* line, const char* hint,
                Tone ring);
  void competitionName(char (&out)[48], uint8_t id);
  const football::CompetitionState* stateOf(uint8_t id);
  uint8_t firstFault(int& count);
  Tone causeTone(football::Cause cause);
  void faultCard(uint8_t id, const football::DatasetStatus& status,
                 const char* title = nullptr);
  void matchProgress(const football::StoredMatch& match);
  void shootout(const football::MatchStore&, const football::StoredMatch&,
                int64_t now);
  void ownHighlight(const football::MatchStore& store,
                    const football::StoredMatch& match, int64_t now);
  void bindOwnMatch(const football::MatchStore& store,
                    const football::StoredMatch& match,
                    const BindContext& context);
  void matchRow(const football::MatchStore& store,
                const football::StoredMatch& match, const BindContext& context,
                bool with_day, MatchRow& out);
  void bindConference(const football::MatchStore& store,
                      const PreparedContent& content,
                      const BindContext& context);
  void bindTable(const football::MatchStore& store,
                 const PreparedContent& content);

  void bindMatchday(const football::MatchStore& store,
                    const PreparedContent& content, const BindContext& context);
  bool networkFault(const BindContext& context) const;
  bool networkUseIp(const BindContext& context) const;
  const char* networkPhase(network::InitStep step) const;
  void networkFaultCard(const BindContext& context);
  Subject statusText(const football::Published& data,
                     const BindContext& context);
  uint32_t statusState(const football::Published& data,
                       const BindContext& context);
  uint32_t bindStatus(const football::Published& data,
                      const BindContext& context);
  const char* imageFailure(images::Failure failure) const;
  bool imageReplace(images::Failure failure) const;
  void imageFaultCard(const images::Status& selected_image);
};
}  // namespace ui
