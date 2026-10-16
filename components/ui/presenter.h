// Private UI composition interface. One controller owns the model, navigation
// and view; a presenter borrows them only during a repository callback.
#pragma once
#include <cstdio>
#include <cstring>

#include "football_screen.h"
#include "football_service.h"
#include "image_service.h"
#include "navigation_policy.h"
#include "network_service.h"

namespace ui {
struct BindContext {
  int64_t utc, now;
  bool valid, connected, ap;
  const network::Status* net;
  uint32_t status_state = 0;
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
  Presenter(const settings::Model& settings, ViewModel& output,
            NavigationState& navigation, PresentationState& presentation,
            std::size_t inputs,
            const football::Published* publication = nullptr,
            const NavigationData* facts = nullptr)
      : model(settings),
        view(output),
        nav(navigation),
        presentation(presentation),
        input_count(inputs),
        published(publication),
        short_names(publication ? &publication->names : nullptr),
        navigation_data(facts) {}
  // Compose under the repository read. After image selection outside that
  // lock, finishStatus applies the one shared warning fingerprint/timer.
  uint32_t bind(const BindContext& context);
  void finishStatus(uint32_t state, int64_t now,
                    const images::Status& selected_image);
  void bindStart(uint8_t step);

 private:
  const settings::Model& model;
  ViewModel& view;
  NavigationState& nav;
  PresentationState& presentation;
  std::size_t input_count;
  const football::Published* published;
  const football::persist::ShortNames* short_names;
  const NavigationData* navigation_data;
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
  void pair(char (&out)[N], const char* format, const football::Goals& score) {
    char text[24];
    std::snprintf(text, sizeof(text), format, score.home, score.away);
    prefix<N - 1>(out, text);
  }
  bool started(const football::StoredMatch& match) const {
    return football::view::started(match.phase);
  }
  void value(char* out, std::size_t size, int n, bool known);
  void value(char* out, std::size_t size, uint8_t n);
  void value(char* out, std::size_t size, int16_t n);
  void localDate(char* out, std::size_t size, int64_t utc);
  void localTime(char* out, std::size_t size, int64_t utc);
  bool entryOf(uint8_t id, settings::Competition& entry);
  void roundCaption(const football::MatchStore& store,
                    const football::StoredMatch& match, bool date);
  void cardText(const char* title, const char* line, const char* hint,
                Tone ring);
  void competitionName(char (&out)[48], uint8_t id);
  const football::CompetitionState* stateOf(uint8_t id);
  bool faulted(uint8_t id);
  uint8_t firstFault(int& count);
  Tone causeTone(football::Cause cause);
  void faultCard(uint8_t id, int64_t now, int64_t utc,
                 const char* title = nullptr);
  void ownHighlight(const football::MatchStore& store,
                    const football::StoredMatch& match, int64_t now);
  void bindOwnMatch(const football::MatchStore& store,
                    const football::StoredMatch& match,
                    const football::view::Shown& shown,
                    const BindContext& context);
  void matchRow(const football::MatchStore& store,
                const football::StoredMatch& match,
                const football::view::Shown& shown, const BindContext& context,
                bool with_day, MatchRow& out);
  void bindConference(const football::MatchStore& store,
                      const football::view::Shown& shown,
                      const football::view::List& members,
                      const BindContext& context);
  void bindTable(const football::MatchStore& store,
                 const TableSelection& selection);
  void bindTables(const football::MatchStore& store,
                  const football::view::Shown& shown, int64_t now, int64_t utc);
  void bindMatchday(const football::MatchStore& store,
                    const football::view::Shown& shown,
                    const BindContext& context);
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
