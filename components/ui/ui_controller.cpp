#include "ui_controller.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>

#include "app_config.h"
#include "conference.h"
#include "display_port.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "event_bus.h"
#include "football_screen.h"
#include "football_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "image_service.h"
#include "input_policy.h"
#include "navigation_policy.h"
#include "network_service.h"
#include "settings_store.h"
#include "time_service.h"
namespace ui {
namespace {
geometry::SafeArea area(0, 0, hw::DisplayShape::kRect, 0);
settings::Model model;
ViewModel view;
cfg::Screen selected = cfg::Screen::kCrest,
            default_screen = cfg::Screen::kCrest;
int scroll = 0, table_rows = 0, own_row = 0;
// Konferenz page chosen with Up/Down (kickoff order) and the page and page
// count of the last bind.
int conference_page = 0, shown_page = 0, shown_pages = 0;
int64_t input_at = 0, scroll_at = 0, render_at = 0, boot_at = 0;
bool good = false, manual = false, scroll_down = true, paged = false;
// The setup card already shows the setup network; no chip on top of it.
bool no_badge = false;
// Start screen from boot until the first data, a failed step, an input, the
// setup network or the demo; it does not return (docs/UI.md "Start screen").
bool starting = true;
// Panel without a backlight: night dims the colours instead.
bool dim_colours = false;
std::size_t input_count = 0;
uint32_t generation = UINT32_MAX, image_revision = 0, network_epoch = 0;
hw::DisplayProfile profile{};
// Screen and reason packed in one word: written by poll(), read by the Web
// task without a lock.
std::atomic<uint32_t> screen_state{0};
struct BindContext {
  int64_t utc, now;
  bool valid, connected, ap;
  const network::Status* net;
};
std::array<InputPolicy, hw::kMaxInputs> buttons{};
StaticQueue_t action_buffer;
uint32_t action_storage[cfg::kEventUiActionSlots];
QueueHandle_t actions = nullptr;
void onAction(events::Event, uint32_t action, void*) {
  if (xQueueSend(actions, &action, 0) != pdPASS)
    ESP_LOGW("input", "pending action dropped");
}
const char* translated(const char* de, const char* en) {
  return model.language ? en : de;
}
// Number or "--", cut to the field size (table fields hold a few digits).
void value(char* out, std::size_t size, const football::Value<int16_t>& n) {
  if (size == 0) return;
  char text[8] = "--";
  if (n.valid()) snprintf(text, sizeof(text), "%d", n.value);
  const std::size_t count = std::min(size - 1, strlen(text));
  memcpy(out, text, count);
  out[count] = 0;
}
const char* name(const football::Team& team) {
  return team.short_name.empty() ? team.name.data : team.short_name.data;
}
bool own(const football::Match& match) { return match.own; }
// Image of a settings reference; a library image that does not exist shows
// the stock image of its role.
void imageSource(uint8_t id, cfg::StockImage stock) {
  if (!images::source(id, stock, view.image, sizeof(view.image)))
    images::source(cfg::kStockImageId, stock, view.image, sizeof(view.image));
}
template <std::size_t N>
void prefix(char (&out)[N + 1], const char* text) {
  football::Text<N> value;
  value.assign(text);
  memcpy(out, value.data, sizeof(out));
}
void changeScreen(int step) {
  const int initial = static_cast<int>(selected);
  for (int n = 1; n <= 4; ++n) {
    const auto next = static_cast<unsigned>((initial + step * n + 8) % 4);
    if (model.screens[next].enabled) {
      selected = static_cast<cfg::Screen>(next);
      break;
    }
  }
  manual = true;
  scroll = 0;
  scroll_down = true;
  paged = false;
  input_at = timekeeping::monotonicMs();
}
void action(Action input) {
  input_at = timekeeping::monotonicMs();
  starting = false;
  if (input == Action::kNext || input == Action::kPrevious) {
    changeScreen(input == Action::kNext ? 1 : -1);
  } else if (input == Action::kDefault) {
    manual = false;
    selected = default_screen;
    scroll = 0;
    scroll_down = true;
    paged = false;
  } else if (input == Action::kShowIp) {
    boot_at = input_at;
  } else if (input == Action::kToggleDirection) {
    scroll_down = !scroll_down;
    scroll_at = input_at;
  } else if (selected == cfg::Screen::kLiveMulti &&
             (input == Action::kUp || input == Action::kDown)) {
    // Page-wise from the page on screen, in kickoff order (OQ-140).
    conference_page = std::clamp(
        (paged ? conference_page : shown_page) +
            (input == Action::kDown ? 1 : -1),
        0, std::max(0, shown_pages - 1));
    paged = true;
    manual = true;
  } else if (selected == cfg::Screen::kTable &&
             (input == Action::kUp || input == Action::kDown)) {
    scroll += input == Action::kDown ? 1 : -1;
    const int first = std::clamp(
        own_row - static_cast<int>(model.table_window) / 2, 0,
        std::max(0, table_rows - static_cast<int>(model.table_window)));
    scroll = std::clamp(
        scroll, -first,
        std::max(0, table_rows - static_cast<int>(model.table_window)) - first);
    scroll_at = input_at;
    manual = true;
  }
}
// Score pair such as "1 : 2", cut to the view field.
template <std::size_t N>
void pair(char (&out)[N], const char* format, const football::Score& score) {
  char text[24];
  snprintf(text, sizeof(text), format, score.home.value, score.away.value);
  prefix<N - 1>(out, text);
}
// "So 01.11." style weekday and date, "15:30" style local time.
void localDate(char* out, std::size_t size, int64_t utc) {
  static constexpr const char* kDe[] = {"So", "Mo", "Di", "Mi",
                                        "Do", "Fr", "Sa"};
  static constexpr const char* kEn[] = {"Sun", "Mon", "Tue", "Wed",
                                        "Thu", "Fri", "Sat"};
  std::tm local{};
  timekeeping::toLocal(static_cast<time_t>(utc), local);
  snprintf(out, size, "%s %02d.%02d.",
           (model.language ? kEn : kDe)[local.tm_wday % 7], local.tm_mday,
           local.tm_mon + 1);
}
void localTime(char* out, std::size_t size, int64_t utc) {
  std::tm local{};
  timekeeping::toLocal(static_cast<time_t>(utc), local);
  snprintf(out, size, "%02d:%02d", local.tm_hour, local.tm_min);
}
// Which side of an own match is the configured team of its competition.
void ownSides(const football::Match& match, bool& home, bool& away) {
  home = away = false;
  if (!match.own || match.route >= model.competitions.size()) return;
  const auto& entry = model.competitions[match.route];
  const auto& source =
      match.provider == entry.primary.provider ? entry.primary : entry.fallback;
  home = source.team.view() == std::string_view(match.home.id.data);
  away = source.team.view() == std::string_view(match.away.id.data);
}
bool started(const football::Match& match) {
  using football::Phase;
  return match.phase == Phase::kLive || match.phase == Phase::kHalfTime ||
         match.phase == Phase::kExtraTime || match.phase == Phase::kPenalties ||
         match.phase == Phase::kFinished || match.phase == Phase::kAbandoned;
}
void cardText(const char* title, const char* line, const char* hint,
              Tone ring) {
  prefix<sizeof(view.card.title) - 1>(view.card.title, title);
  view.card.line_count = 0;
  view.card.hint_count = 0;
  if (line && line[0])
    prefix<sizeof(view.card.lines[0]) - 1>(
        view.card.lines[view.card.line_count++], line);
  if (hint && hint[0])
    prefix<sizeof(view.card.hints[0]) - 1>(
        view.card.hints[view.card.hint_count++], hint);
  view.layout = Layout::kCard;
  view.rim.kind = RimKind::kRing;
  view.rim.tone = ring;
}
// Start screen: club name, the running step (0 WLAN, 1 time, 2 data) and the
// three steps as rim segments. Caption: the version, then the data provider.
void bindStart(uint8_t step) {
  view.style = model.screens[static_cast<unsigned>(cfg::Screen::kCrest)];
  const settings::Competition* source = nullptr;
  for (const auto& entry : model.competitions)
    if (entry.active && source == nullptr) source = &entry;
  char club[48] = "Fussball";
  for (const auto& entry : model.competitions)
    if (!entry.team_name.empty()) {
      snprintf(club, sizeof(club), "%.*s",
               static_cast<int>(entry.team_name.view().size()),
               entry.team_name.data.data());
      break;
    }
  const char* line =
      step == 0   ? translated("Verbinde WLAN …", "Connecting WiFi …")
      : step == 1 ? translated("Hole Uhrzeit …", "Getting time …")
                  : translated("Lade Daten …", "Loading data …");
  cardText(club, line,
           step == 2 && source
               ? football::providerName(source->primary.provider)
               : esp_app_get_description()->version,
           Tone::kAccent);
  view.rim.kind = RimKind::kSegments;
  view.rim.done = step;
  view.rim.current = step;
  no_badge = true;
}
// Latest event of the own match, rotating through the event list.
void ownHighlight(const football::Snapshot& data, const football::Match& match,
                  int64_t now) {
  const unsigned event_step =
      match.event_count ? now / cfg::kHighlightRotateMs % match.event_count : 0;
  for (unsigned offset = 0; offset < match.event_count; ++offset) {
    const unsigned i =
        match.event_count - 1 - (event_step + offset) % match.event_count;
    const auto& e = data.events[match.events[i]];
    if (e.removed) continue;
    char minute[12];
    value(minute, sizeof(minute), e.minute);
    const char* label =
        e.shootout ? translated("Elfmeterschießen", "Shootout")
        : e.type == football::EventType::kOwnGoal
            ? translated("Eigentor", "Own goal")
        : e.type == football::EventType::kGoal ? translated("Tor", "Goal")
        : e.type == football::EventType::kPenaltyGoal
            ? translated("Elfmeter", "Penalty")
        : e.type == football::EventType::kPenaltyMissed
            ? translated("Verschossen", "Missed penalty")
        : e.type == football::EventType::kYellow ? translated("Gelb", "Yellow")
        : e.type == football::EventType::kYellowRed
            ? translated("Gelb-Rot", "Second yellow")
        : e.type == football::EventType::kRed ? translated("Rot", "Red")
        : e.type == football::EventType::kSubstitution
            ? translated("Wechsel", "Substitution")
        : e.type == football::EventType::kInjury
            ? translated("Verletzung", "Injury")
            : "VAR";
    char player[41];
    if (e.type == football::EventType::kSubstitution && !e.player_in.empty()) {
      char in[19], out[19];
      prefix<18>(in, e.player_in.data);
      prefix<18>(out, e.player_out.data);
      snprintf(player, sizeof(player), "%s > %s", out, in);
    } else
      prefix<40>(player, e.player.data);
    snprintf(view.own.highlight, sizeof(view.own.highlight),
             player[0] ? "%s' %s  %s" : "%s' %s", minute, label, player);
    return;
  }
}
void bindOwnMatch(const football::Snapshot& data, const football::Match& match,
                  const BindContext& context) {
  using football::Phase;
  view.layout = Layout::kOwnMatch;
  auto& own = view.own;
  // Caption: competition · round.
  const char* competition =
      match.route < model.competitions.size()
          ? model.competitions[match.route].competition_name.data.data()
          : "";
  const std::size_t competition_length =
      match.route < model.competitions.size()
          ? model.competitions[match.route].competition_name.view().size()
          : 0;
  char round[40]{};
  if (!match.round_label.empty())
    prefix<39>(round, match.round_label.data);
  else if (match.round.valid())
    snprintf(round, sizeof(round),
             model.language ? "Matchday %u" : "%u. Spieltag",
             static_cast<unsigned>(match.round.value));
  snprintf(view.caption, sizeof(view.caption), "%.*s%s%s",
           static_cast<int>(competition_length), competition,
           competition_length && round[0] ? " \xC2\xB7 " : "", round);
  prefix<40>(own.home, name(match.home));
  prefix<40>(own.away, name(match.away));
  ownSides(match, own.own_home, own.own_away);
  const bool before = !started(match);
  if (match.principal.valid() && !before)
    pair(own.centre, "%d : %d", match.principal);
  else if (match.kickoff.valid())
    localTime(own.centre, sizeof(own.centre), match.kickoff.value);
  else
    snprintf(own.centre, sizeof(own.centre), "-- : --");
  auto& rim = view.rim;
  rim.kind = RimKind::kMatch;
  rim.extra = true;
  if (before) {
    if (match.kickoff.valid())
      localDate(own.pill, sizeof(own.pill), match.kickoff.value);
    own.pill_tone = Tone::kDim;
    own.pill_text = Tone::kText;
    own.highlight_tone = Tone::kDim;
    if (match.kickoff.valid() && context.valid &&
        match.kickoff.value > context.utc) {
      const int64_t left = match.kickoff.value - context.utc;
      const long days = static_cast<long>(left / 86400),
                 hours = static_cast<long>(left / 3600),
                 minutes = static_cast<long>((left + 59) / 60);
      if (days >= 1)
        snprintf(own.highlight, sizeof(own.highlight),
                 model.language ? "Kickoff in %ld day%s" : "Anstoß in %ld %s",
                 days,
                 model.language ? (days == 1 ? "" : "s")
                                : (days == 1 ? "Tag" : "Tagen"));
      else if (hours >= 1)
        snprintf(own.highlight, sizeof(own.highlight), "%s %ld %s",
                 translated("Anstoß in", "Kickoff in"), hours,
                 translated("Std.", "h"));
      else
        snprintf(own.highlight, sizeof(own.highlight), "%s %ld %s",
                 translated("Anstoß in", "Kickoff in"), minutes,
                 translated("Min.", "min"));
    }
    return;
  }
  ownHighlight(data, match, context.now);
  if (match.shootout.valid())
    snprintf(own.pill, sizeof(own.pill), "%s %d : %d",
             translated("i.E.", "pens"), match.shootout.home.value,
             match.shootout.away.value);
  else if (match.phase == Phase::kFinished)
    snprintf(own.pill, sizeof(own.pill), "%s",
             translated("Beendet", "Full time"));
  else if (match.phase == Phase::kHalfTime)
    snprintf(own.pill, sizeof(own.pill), "%s",
             translated("Halbzeit", "Half time"));
  else if (match.phase == Phase::kPenalties)
    snprintf(own.pill, sizeof(own.pill), "%s",
             translated("Elfmeterschießen", "Shootout"));
  else if (match.minute.valid())
    snprintf(own.pill, sizeof(own.pill), "%s%d'",
             match.minute.source == football::Source::kEstimated ? "~" : "",
             match.minute.value);
  if (match.phase == Phase::kFinished) {
    own.pill_tone = own.pill_text = Tone::kDim;
    rim.minute = 90;
    rim.finished = true;
  } else if (match.phase == Phase::kHalfTime) {
    rim.minute = 45;
  } else if (match.phase == Phase::kPenalties) {
    rim.minute = 120;
  } else if (match.minute.valid()) {
    rim.minute = match.minute.value;
  }
  rim.gap = rim.minute >= 0;
}
void bindConference(const football::Snapshot& data,
                    const football::ConferenceList& members,
                    const BindContext& context) {
  const auto now = context.now;
  football::ConferenceRequest request;
  request.mode = model.conference_mode;
  request.pin_own = model.pin_own_match;
  request.manual = paged;
  request.per_page = model.visible_matches;
  request.page = paged ? static_cast<unsigned>(conference_page)
                       : static_cast<unsigned>(now / cfg::kConferencePageMs);
  const auto page = football::conferencePage(data, members, request);
  shown_page = page.page;
  shown_pages = page.pages;
  if (page.row_count == 0) {
    char next[48]{};
    int64_t kickoff = INT64_MAX;
    for (unsigned i = 0; context.valid && i < data.match_count; ++i) {
      const auto& match = data.matches[i];
      if (match.kickoff.valid() && match.kickoff.value > context.utc)
        kickoff = std::min(kickoff, match.kickoff.value);
    }
    if (kickoff != INT64_MAX) {
      char date[16], time[8];
      localDate(date, sizeof(date), kickoff);
      localTime(time, sizeof(time), kickoff);
      // Weekday only: "Sa 15:30".
      char* space = strchr(date, ' ');
      if (space) *space = 0;
      snprintf(next, sizeof(next), "%s %s %s",
               translated("Nächstes:", "Next:"), date, time);
    }
    cardText(translated("Konferenz", "Conference"),
             translated("Keine Spiele im Zeitfenster", "No matches in window"),
             next, Tone::kNeutral);
    return;
  }
  view.layout = Layout::kConference;
  view.page = page.page;
  view.pages = page.pages;
  view.slots = page.pages > 1 ? model.visible_matches : page.row_count;
  if (page.pages > 1)
    snprintf(view.caption, sizeof(view.caption), "%s \xC2\xB7 %u/%u",
             translated("Konferenz", "Conference"), page.page + 1u,
             static_cast<unsigned>(page.pages));
  else
    snprintf(view.caption, sizeof(view.caption), "%s",
             translated("Konferenz", "Conference"));
  const unsigned rows = std::min<unsigned>(page.row_count, view.matches.size());
  for (unsigned row = 0; row < rows; ++row) {
    const auto& match = data.matches[page.rows[row]];
    auto& out = view.matches[row];
    prefix<40>(out.home, name(match.home));
    prefix<40>(out.away, name(match.away));
    ownSides(match, out.own_home, out.own_away);
    out.own = match.own;
    out.goal = match.score_changed_ms > 0 &&
               now - match.score_changed_ms < cfg::kGoalFlashS * 1000;
    if (match.principal.valid() && started(match)) {
      pair(out.centre, "%d:%d", match.principal);
      out.centre_tone = Tone::kText;
    } else if (match.kickoff.valid() && !started(match)) {
      localTime(out.centre, sizeof(out.centre), match.kickoff.value);
      out.centre_tone = Tone::kDim;
    } else {
      snprintf(out.centre, sizeof(out.centre), "--:--");
      out.centre_tone = Tone::kDim;
    }
  }
  view.row_count = rows;
  // Rim: progress of the own match of the Konferenz.
  view.rim.kind = RimKind::kMatch;
  view.rim.gap = true;
  for (unsigned i = 0; i < members.count; ++i) {
    const auto& match = data.matches[members.index[i]];
    if (!match.own) continue;
    if (match.phase == football::Phase::kFinished) {
      view.rim.minute = 90;
      view.rim.finished = true;
    } else if (match.phase == football::Phase::kHalfTime) {
      view.rim.minute = 45;
    } else if (started(match) && match.minute.valid()) {
      view.rim.minute = std::min<int16_t>(match.minute.value, 90);
    }
    break;
  }
}
void bindTable(const football::Snapshot& data) {
  const char* competition = nullptr;
  for (const auto& entry : model.competitions)
    if (entry.active) {
      competition = entry.competition_name.empty()
                        ? nullptr
                        : entry.competition_name.data.data();
      if (competition) {
        snprintf(view.caption, sizeof(view.caption), "%.*s",
                 static_cast<int>(entry.competition_name.view().size()),
                 competition);
      }
      break;
    }
  if (!competition)
    snprintf(view.caption, sizeof(view.caption), "%s",
             translated("Tabelle", "Table"));
  table_rows = data.table_count;
  own_row = 0;
  for (int i = 0; i < table_rows; ++i)
    if (data.table[i].own) own_row = i;
  const int count = std::min<int>(model.table_window, table_rows);
  if (count == 0) {
    char line[sizeof(view.caption) + 24];
    if (competition)
      snprintf(line, sizeof(line), "%s %s", view.caption,
               translated("hat keine Tabelle", "has no table"));
    else
      snprintf(line, sizeof(line), "%s",
               translated("Keine Tabelle", "No table"));
    cardText(translated("Tabelle", "Table"), line, nullptr, Tone::kNeutral);
    return;
  }
  view.layout = Layout::kTable;
  snprintf(view.columns[0], sizeof(view.columns[0]), "%s",
           translated("Pkt", "Pts"));
  snprintf(view.columns[1], sizeof(view.columns[1]), "%s",
           translated("Diff", "GD"));
  snprintf(view.columns[2], sizeof(view.columns[2]), "%s",
           translated("Sp", "P"));
  const int first = std::clamp(own_row - count / 2 + scroll, 0,
                               std::max(0, table_rows - count));
  view.row_count = count;
  for (int i = 0; i < count; ++i) {
    const auto& standing = data.table[first + i];
    auto& row = view.table[i];
    prefix<40>(row.name, name(standing.team));
    if (standing.position.valid())
      value(row.position, sizeof(row.position), standing.position);
    else
      value(row.position, sizeof(row.position),
            football::Value<int16_t>{static_cast<int16_t>(first + i + 1),
                                     football::Source::kEstimated});
    value(row.played, sizeof(row.played), standing.played);
    value(row.points, sizeof(row.points), standing.points);
    if (standing.difference.valid())
      snprintf(row.difference, sizeof(row.difference),
               standing.difference.value > 0 ? "+%d" : "%d",
               standing.difference.value);
    else
      snprintf(row.difference, sizeof(row.difference), "--");
    row.own = standing.own;
  }
  view.rim.kind = RimKind::kScroll;
  view.rim.first = static_cast<uint8_t>(first);
  view.rim.count = static_cast<uint8_t>(count);
  view.rim.total = static_cast<uint8_t>(table_rows);
}
void bind(const football::Snapshot& data, void* raw_context) {
  const auto& context = *static_cast<BindContext*>(raw_context);
  // Rebuilt in place: `view = {}` would add a ~1.9 KB temporary to the
  // main task stack.
  new (&view) ViewModel();
  no_badge = false;
  const auto utc = context.utc;
  const bool valid = context.valid;
  // Konferenz members; the demo shows every match in kickoff order.
  football::ConferenceList members;
  if (model.demo) {
    members = football::conferenceAll(data);
  } else if (valid) {
    members = football::conferenceMembers(
        data, utc,
        {model.window_before, model.window_after, model.whole_matchday});
  }
  // Every active competition opens the Konferenz (OQ-146).
  const bool window = members.count > 0;
  bool own_window = false;
  const football::Match* own_match = nullptr;
  for (unsigned i = 0; i < data.match_count; ++i) {
    const auto& match = data.matches[i];
    bool active =
        (model.demo && match.own) ||
        (valid && football::matchWindow(match, utc, model.window_before,
                                        model.window_after));
    if (own(match) &&
        (own_match == nullptr || (active && !own_window) ||
         (!own_window && match.kickoff.valid() && own_match->kickoff.valid() &&
          std::llabs(match.kickoff.value - utc) <
              std::llabs(own_match->kickoff.value - utc)))) {
      own_match = &match;
      own_window |= active;
    }
  }
  const bool chosen = input_count && manual &&
                      model.screens[static_cast<unsigned>(selected)].enabled;
  auto navigation = resolve(model, {data.configured, window, own_window, manual,
                                    input_count, selected});
  default_screen = navigation.default_screen;
  selected = navigation.selected;
  const Reason reason = chosen             ? Reason::kManual
                        : !data.configured ? Reason::kSetup
                        : own_window       ? Reason::kOwnMatch
                        : window           ? Reason::kMatchday
                                           : Reason::kIdle;
  screen_state.store(
      static_cast<uint32_t>(selected) | static_cast<uint32_t>(reason) << 8,
      std::memory_order_relaxed);
  view.screen = selected;
  view.style = model.screens[static_cast<unsigned>(selected)];
  const auto now = context.now;
  if (starting) {
    const auto wifi = context.net->state;
    bool active = false;
    for (const auto& entry : model.competitions) active |= entry.active;
    // A failed step shows its state card instead (docs/UI.md "State cards").
    starting = active && !model.demo && !context.ap && data.fetched_ms == 0 &&
               data.error.empty() && wifi != network::State::kBackoff &&
               wifi != network::State::kBackoffAuth &&
               wifi != network::State::kWaitSsid &&
               wifi != network::State::kApOnly;
    if (starting) {
      bindStart(!context.connected ? 0 : !valid ? 1 : 2);
      return;
    }
  }
  const bool stale =
      data.stale ||
      (data.fetched_ms > 0 && now - data.fetched_ms > cfg::kDataStaleS * 1000);
  // Bottom status band (docs/UI.md "Status band").
  if (model.demo) {
    snprintf(view.status, sizeof(view.status), "Demo");
    view.status_tone = Tone::kDim;
  } else if (!context.connected) {
    snprintf(view.status, sizeof(view.status), "%s",
             translated("Kein WLAN", "No WiFi"));
    view.status_tone = Tone::kWarn;
  } else if (!valid) {
    snprintf(view.status, sizeof(view.status), "%s",
             translated("Warte auf Uhrzeit", "Waiting for time"));
    view.status_tone = Tone::kWarn;
  } else if (stale) {
    if (data.fetched_ms > 0) {
      // A competition that failed keeps earlier data: show the oldest.
      char time[8];
      localTime(time, sizeof(time),
                utc - (now - football::oldestFetch(data)) / 1000);
      snprintf(view.status, sizeof(view.status), "%s %s",
               translated("Stand", "As of"), time);
    } else
      snprintf(view.status, sizeof(view.status), "%s",
               translated("Daten veraltet", "Data stale"));
    view.status_tone = Tone::kWarn;
  } else if (!data.error.empty()) {
    prefix<sizeof(view.status) - 1>(view.status, data.error.data);
    view.status_tone = Tone::kError;
  }
  const bool data_screen = selected != cfg::Screen::kCrest;
  if (!data.configured && context.ap) {
    // First setup: how to reach the setup page.
    const auto& net = *context.net;
    char ssid[48];
    snprintf(ssid, sizeof(ssid), "\xC2\xBB %s", net.ap_ssid);
    cardText(translated("Einrichtung", "Setup"),
             translated("Mit WLAN verbinden:", "Join WiFi:"),
             translated("dann im Browser öffnen:", "then open in a browser:"),
             Tone::kAccent);
    prefix<sizeof(view.card.lines[1]) - 1>(view.card.lines[1], ssid);
    view.card.accent[1] = true;
    view.card.line_count = 2;
    prefix<sizeof(view.card.hints[1]) - 1>(view.card.hints[1], net.ap_ip);
    view.card.hint_count = 2;
    no_badge = true;
  } else if (data_screen && data.fetched_ms == 0 && !model.demo) {
    // Data never loaded: say why instead of an empty screen.
    if (!context.connected)
      cardText(translated("Kein WLAN", "No WiFi"),
               translated("Heimnetz nicht erreichbar", "Home network not found"),
               translated("Neuer Versuch läuft", "Retrying"), Tone::kWarn);
    else if (!valid)
      cardText(translated("Warte auf Uhrzeit", "Waiting for time"),
               translated("Zeitserver wird gefragt", "Asking the time server"),
               nullptr, Tone::kWarn);
    else {
      // After a failed attempt: when the next one starts.
      char retry[40]{};
      if (!data.error.empty() && data.retry_ms > now) {
        char time[8];
        localTime(time, sizeof(time), utc + (data.retry_ms - now + 999) / 1000);
        snprintf(retry, sizeof(retry), "%s %s",
                 translated("Neuer Versuch", "Retry at"), time);
      }
      cardText(translated("Keine Daten", "No data"),
               data.error.empty() ? translated("Lade Daten", "Loading data")
                                  : data.error.data,
               retry, data.error.empty() ? Tone::kNeutral : Tone::kError);
    }
  } else if (selected == cfg::Screen::kLiveSingle) {
    if (own_match != nullptr)
      bindOwnMatch(data, *own_match, context);
    else
      cardText(translated("Eigenes Spiel", "Your match"),
               translated("Kein Spiel", "No match"), nullptr, Tone::kNeutral);
  } else if (selected == cfg::Screen::kLiveMulti) {
    bindConference(data, members, context);
  } else if (selected == cfg::Screen::kTable) {
    bindTable(data);
  }
}
// Draws the view with the settings' rotation and brightness; false if the
// display lock timed out.
bool draw(uint32_t lock_timeout_ms) {
  display::setBrightness(view.night ? model.night_brightness
                                    : model.brightness);
  if (!display::lock(lock_timeout_ms)) return false;
  display::setRotation(model.rotation);
  if (image_revision != images::revision()) {
    image_revision = images::revision();
    display::dropImageCache(view.image);
  }
  view.image_revision = image_revision;
  auto rotated = profile;
  if (model.rotation % 2) std::swap(rotated.width, rotated.height);
  area = geometry::SafeArea::forDisplay(rotated, cfg::kContentMarginDivisor);
  good = render(view, area);
  display::unlock();
  return true;
}
}  // namespace
esp_err_t init(const hw::DisplayProfile& display, std::size_t inputs,
               bool backlight) {
  dim_colours = !backlight;
  area = geometry::SafeArea::forDisplay(display, cfg::kContentMarginDivisor);
  profile = display;
  input_count = std::min(inputs, buttons.size());
  boot_at = timekeeping::monotonicMs();
  if (!area.isValid()) return ESP_ERR_INVALID_ARG;
  model = settings::current();
  actions = xQueueCreateStatic(cfg::kEventUiActionSlots, sizeof(uint32_t),
                               reinterpret_cast<uint8_t*>(action_storage),
                               &action_buffer);
  if (!actions) return ESP_ERR_NO_MEM;
  return events::subscribe(events::Event::kUiAction, onAction, nullptr);
}
void input(std::size_t index, bool active) {
  if (index >= input_count) return;
  const auto role = model.input_roles[index];
  // Only the mandatory input waits for a second press (double = Previous).
  const auto emitted =
      buttons[index].update(active, timekeeping::monotonicMs(),
                            role == cfg::InputRole::kMandatory,
                            model.scroll_repeat_ms);
  for (unsigned event = 0; event <= static_cast<unsigned>(RawInput::kRepeat);
       ++event) {
    if (!(emitted & (1u << event))) continue;
    const auto semantic =
        mapInput(role, static_cast<RawInput>(event), scroll_down);
    if (semantic != Action::kNone)
      events::post(events::Event::kUiAction, static_cast<uint32_t>(semantic));
  }
}
void poll() {
  const int64_t now = timekeeping::monotonicMs();
  uint32_t pending;
  while (actions && xQueueReceive(actions, &pending, 0) == pdPASS)
    action(static_cast<Action>(pending));
  if (now - render_at < cfg::kUiRefreshMs) return;
  render_at = now;
  if (generation != settings::generation()) {
    generation = settings::generation();
    model = settings::current();
    manual = false;
  }
  if (manual && model.manual_return_s > 0 &&
      now - input_at >= model.manual_return_s * 1000) {
    manual = false;
    scroll = 0;
    paged = false;
  }
  if (model.scroll_reset_s > 0 &&
      now - scroll_at >= model.scroll_reset_s * 1000)
    scroll_down = true;
  time_t utc = 0;
  const auto net = network::status();
  BindContext context{0, now, timekeeping::utcNow(utc), network::ready(),
                      net.ap, &net};
  context.utc = utc;
  football::read(bind, &context);
  std::tm local{};
  view.night = model.night_enabled && timekeeping::localNow(local) &&
               timekeeping::contains(
                   {model.night_start, model.night_end},
                   static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min));
  // Night brightness as an amount of light: colour value = level^(1/2.2).
  if (view.night && dim_colours)
    view.night_colour = static_cast<uint8_t>(
        std::lround(255.0f * std::pow(model.night_brightness / 100.0f,
                                      1.0f / 2.2f)));
  if (starting) {
    // The start screen has no image.
  } else if (selected == cfg::Screen::kCrest) {
    uint8_t id = cfg::kStockImageId;
    if (!model.slides.empty()) {
      // Slides of images that no longer exist are skipped.
      const auto present = images::present();
      uint8_t shown[cfg::kSlideMax], count = 0;
      for (const uint8_t slide : model.slides)
        if (slide == cfg::kStockImageId ||
            (slide <= cfg::kImageLibraryMax && (present >> slide) & 1))
          shown[count++] = slide;
      if (count > 0) {
        const unsigned index = (now / (model.slideshow_s * 1000)) % count;
        const uint8_t slide = shown[index];
        id = slide;
        if (count > 1 && view.layout == Layout::kImage) {
          view.page = static_cast<uint8_t>(index);
          view.pages = count;
        }
      }
    }
    imageSource(id, cfg::kStockCrest);
  } else if (model.backgrounds && view.style.background) {
    imageSource(view.style.image, cfg::kStockBackground);
  }
  if (net.epoch != network_epoch) {
    network_epoch = net.epoch;
    if (net.connected) boot_at = now;
  }
  view.badge_bottom = model.ip_badge_bottom;
  if (!no_badge && ((net.ap && model.ip_badge_ap_permanent) ||
                    model.ip_badge_s == 0 ||
                    now - boot_at < model.ip_badge_s * 1000)) {
    if (net.ap) {
      snprintf(view.badge[0], sizeof(view.badge[0]), "%s", net.ap_ssid);
      snprintf(view.badge[1], sizeof(view.badge[1]), "%s", net.ap_ip);
      view.badge_lines = 2;
    } else if (net.connected) {
      snprintf(view.badge[0], sizeof(view.badge[0]), "%s", net.ip);
      view.badge_lines = 1;
    }
  }
  draw(cfg::kStatusLockTimeoutMs);
}
bool showStartScreen(const hw::DisplayProfile& display, bool backlight) {
  dim_colours = !backlight;
  profile = display;
  model = settings::current();
  new (&view) ViewModel();
  bindStart(0);
  return draw(cfg::kBootScreenLockTimeoutMs) && good;
}
bool healthy() { return good; }
Status status() {
  const uint32_t word = screen_state.load(std::memory_order_relaxed);
  return {static_cast<cfg::Screen>(word & 0xff),
          static_cast<Reason>(word >> 8 & 0xff)};
}
const char* reasonName(Reason reason) {
  switch (reason) {
    case Reason::kSetup: return "setup";
    case Reason::kManual: return "manual";
    case Reason::kOwnMatch: return "own_match";
    case Reason::kMatchday: return "matchday";
    case Reason::kIdle: return "idle";
  }
  return "idle";
}
}  // namespace ui
