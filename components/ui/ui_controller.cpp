#include "ui_controller.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app_config.h"
#include "display_port.h"
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
int64_t input_at = 0, scroll_at = 0, render_at = 0, boot_at = 0;
bool good = false, manual = false, scroll_down = true;
std::size_t input_count = 0;
uint32_t generation = UINT32_MAX, image_revision = 0, network_epoch = 0;
hw::DisplayProfile profile{};
struct BindContext {
  int64_t utc, now;
  bool valid, connected;
};
football::Value<int64_t> display_kickoff;
std::array<InputPolicy, hw::kMaxInputs> buttons{};
StaticQueue_t action_buffer;
uint32_t action_storage[cfg::kEventUiActionSlots];
QueueHandle_t actions = nullptr;
uint16_t reset_remaining = 0;
void onAction(events::Event, uint32_t action, void*) {
  if (xQueueSend(actions, &action, 0) != pdPASS)
    ESP_LOGW("input", "pending action dropped");
}
const char* translated(const char* de, const char* en) {
  return model.language ? en : de;
}
void value(char* out, std::size_t size, const football::Value<int16_t>& n) {
  if (n.valid())
    snprintf(out, size, "%d", n.value);
  else
    snprintf(out, size, "--");
}
void scoreText(char* out, std::size_t size, const football::Score& score) {
  if (score.valid())
    snprintf(out, size, "%d : %d", score.home.value, score.away.value);
  else
    snprintf(out, size, "-- : --");
}
const char* name(const football::Team& team) {
  return team.short_name.empty() ? team.name.data : team.short_name.data;
}
bool own(const football::Match& match) { return match.own; }
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
  input_at = timekeeping::monotonicMs();
}
void action(Action input) {
  input_at = timekeeping::monotonicMs();
  if (input == Action::kNext || input == Action::kPrevious) {
    changeScreen(input == Action::kNext ? 1 : -1);
  } else if (input == Action::kDefault) {
    manual = false;
    selected = default_screen;
    scroll = 0;
    scroll_down = true;
  } else if (input == Action::kToggleDirection) {
    scroll_down = !scroll_down;
    scroll_at = input_at;
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
void bind(const football::Snapshot& data, void* raw_context) {
  const auto& context = *static_cast<BindContext*>(raw_context);
  view = {};
  display_kickoff = {};
  const auto utc = context.utc;
  const bool valid = context.valid;
  bool window = model.demo || (valid && football::conferenceWindow(
                                            data, utc, model.window_before,
                                            model.window_after));
  bool own_window = false;
  uint8_t conference_route = 0;
  bool have_conference = false;
  const football::Match* own_match = nullptr;
  for (unsigned i = 0; i < data.match_count; ++i) {
    const auto& match = data.matches[i];
    bool active =
        (model.demo && match.own) ||
        (valid && football::matchWindow(match, utc, model.window_before,
                                        model.window_after));
    if (active && !have_conference) {
      conference_route = match.route;
      have_conference = true;
    }
    if (own(match) &&
        (own_match == nullptr || (active && !own_window) ||
         (!own_window && match.kickoff.valid() && own_match->kickoff.valid() &&
          std::llabs(match.kickoff.value - utc) <
              std::llabs(own_match->kickoff.value - utc)))) {
      own_match = &match;
      own_window |= active;
    }
  }
  if (own_window && own_match) conference_route = own_match->route;
  auto navigation = resolve(model, {data.configured, window, own_window, manual,
                                    input_count, selected});
  default_screen = navigation.default_screen;
  selected = navigation.selected;
  view.screen = selected;
  view.style = model.screens[static_cast<unsigned>(selected)];
  const auto now = context.now;
  const bool stale =
      data.stale ||
      (data.fetched_ms > 0 && now - data.fetched_ms > cfg::kDataStaleS * 1000);
  prefix<sizeof(view.status) - 1>(
      view.status, model.demo           ? translated("Demo", "Demo")
                   : !context.connected ? translated("Kein WLAN", "No WiFi")
                   : !valid
                       ? translated("Warte auf Uhrzeit", "Waiting for time")
                   : stale ? translated("Daten veraltet", "Data stale")
                   : data.error.empty() ? ""
                                        : data.error.data);
  if (selected == cfg::Screen::kLiveSingle) {
    snprintf(view.title, sizeof(view.title), "%s",
             translated("Eigenes Spiel", "Your match"));
    if (own_match != nullptr) {
      scoreText(view.score, sizeof(view.score), own_match->principal);
      char home[41], away[41];
      prefix<40>(home, name(own_match->home));
      prefix<40>(away, name(own_match->away));
      snprintf(view.teams, sizeof(view.teams), "%s - %s", home, away);
      if (own_match->shootout.valid())
        snprintf(view.minute, sizeof(view.minute), "%d : %d %s",
                 own_match->shootout.home.value, own_match->shootout.away.value,
                 translated("i.E.", "pens"));
      else if (own_match->phase == football::Phase::kFinished)
        snprintf(view.minute, sizeof(view.minute), "%s",
                 translated("Beendet", "Full time"));
      else if (own_match->phase == football::Phase::kHalfTime)
        snprintf(view.minute, sizeof(view.minute), "%s",
                 translated("Halbzeit", "Half time"));
      else if (own_match->phase == football::Phase::kPenalties)
        snprintf(view.minute, sizeof(view.minute), "%s",
                 translated("Elfmeterschießen", "Shootout"));
      else if (own_match->minute.valid())
        snprintf(
            view.minute, sizeof(view.minute), "%s%d'",
            own_match->minute.source == football::Source::kEstimated ? "~" : "",
            own_match->minute.value);
      else
        display_kickoff = own_match->kickoff;
      // Present the bounded event list in one safe-area band. Rotating avoids
      // growing the object tree or hiding earlier cards/substitutions.
      const unsigned event_step =
          own_match->event_count
              ? now / cfg::kHighlightRotateMs % own_match->event_count
              : 0;
      for (unsigned offset = 0; offset < own_match->event_count; ++offset) {
        const unsigned i = own_match->event_count - 1 -
                           (event_step + offset) % own_match->event_count;
        const auto& e = data.events[own_match->events[i]];
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
            : e.type == football::EventType::kYellow
                ? translated("Gelb", "Yellow")
            : e.type == football::EventType::kYellowRed
                ? translated("Gelb-Rot", "Second yellow")
            : e.type == football::EventType::kRed ? translated("Rot", "Red")
            : e.type == football::EventType::kSubstitution
                ? translated("Wechsel", "Substitution")
            : e.type == football::EventType::kInjury
                ? translated("Verletzung", "Injury")
                : "VAR";
        char player[41];
        if (e.type == football::EventType::kSubstitution &&
            !e.player_in.empty()) {
          char in[19], out[19];
          prefix<18>(in, e.player_in.data);
          prefix<18>(out, e.player_out.data);
          snprintf(player, sizeof(player), "%s > %s", out, in);
        } else
          prefix<40>(player, e.player.data);
        snprintf(view.highlight, sizeof(view.highlight), "%s' %s %s", minute,
                 label, player);
        break;
      }
    } else
      snprintf(view.score, sizeof(view.score), "%s",
               translated("Kein Spiel", "No match"));
  } else if (selected == cfg::Screen::kLiveMulti) {
    snprintf(view.title, sizeof(view.title), "%s",
             translated("Konferenz", "Conference"));
    std::array<uint8_t, cfg::kMatches> order{};
    const football::Match* conference_own = nullptr;
    for (unsigned i = 0; i < data.match_count; ++i) {
      const auto& m = data.matches[i];
      if (m.route != conference_route || !m.own) continue;
      if (!conference_own ||
          (m.kickoff.valid() && conference_own->kickoff.valid() &&
           std::llabs(m.kickoff.value - utc) <
               std::llabs(conference_own->kickoff.value - utc)))
        conference_own = &m;
    }
    unsigned conference_count = 0;
    for (unsigned i = 0; i < data.match_count; ++i) {
      const auto& match = data.matches[i];
      if (&match == conference_own ||
          (match.route == conference_route &&
           (!valid || !match.kickoff.valid() ||
            std::llabs(match.kickoff.value - utc) <=
                cfg::kConferenceHorizonHours * 3600LL)))
        order[conference_count++] = i;
    }
    std::sort(order.begin(), order.begin() + conference_count,
              [&](uint8_t a, uint8_t b) {
                return data.matches[a].score_changed_ms >
                       data.matches[b].score_changed_ms;
              });
    const unsigned limit =
        std::min<unsigned>(model.visible_matches, conference_count);
    const unsigned centre = limit / 2;
    unsigned next = 0;
    for (unsigned row = 0; row < limit; ++row) {
      const football::Match* match = nullptr;
      if (conference_own && row == centre)
        match = conference_own;
      else {
        while (next < conference_count &&
               &data.matches[order[next]] == conference_own)
          ++next;
        if (next < conference_count) match = &data.matches[order[next++]];
      }
      if (!match) break;
      char score[24];
      scoreText(score, sizeof(score), match->principal);
      char home[21], away[21];
      prefix<20>(home, name(match->home));
      prefix<20>(away, name(match->away));
      snprintf(view.rows[row].text, sizeof(view.rows[row].text), "%s %s %s",
               home, score, away);
      view.rows[row].own = own(*match);
      ++view.row_count;
    }
    if (!view.row_count)
      snprintf(view.status, sizeof(view.status), "%s",
               translated("Keine Spiele", "No fixtures"));
  } else if (selected == cfg::Screen::kTable) {
    snprintf(view.title, sizeof(view.title), "%s",
             translated("Tabelle", "Table"));
    table_rows = data.table_count;
    own_row = 0;
    for (int i = 0; i < table_rows; ++i)
      if (data.table[i].own) own_row = i;
    const int count = std::min<int>(model.table_window, table_rows);
    const int first = std::clamp(own_row - count / 2 + scroll, 0,
                                 std::max(0, table_rows - count));
    view.row_count = count;
    for (int i = 0; i < count; ++i) {
      const auto& standing = data.table[first + i];
      char position[12], points[12];
      value(position, sizeof(position), standing.position);
      value(points, sizeof(points), standing.points);
      char team[51];
      prefix<50>(team, name(standing.team));
      snprintf(view.rows[i].text, sizeof(view.rows[i].text), "%s  %s  %s",
               position, team, points);
      view.rows[i].own = standing.own;
    }
    if (count == 0)
      snprintf(view.status, sizeof(view.status), "%s",
               translated("Keine Tabelle", "No table"));
  } else
    snprintf(view.title, sizeof(view.title), "%s",
             data.configured ? "" : translated("Fussball", "Football"));
}
}  // namespace
esp_err_t init(const hw::DisplayProfile& display, std::size_t inputs) {
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
  const auto emitted = buttons[index].update(
      active, timekeeping::monotonicMs(), index == 0, model.scroll_repeat_ms);
  for (unsigned event = 0; event <= static_cast<unsigned>(RawInput::kRepeat);
       ++event) {
    if (!(emitted & (1u << event))) continue;
    const auto semantic =
        mapInput(input_count, index, static_cast<RawInput>(event), scroll_down);
    if (semantic != Action::kNone)
      events::post(events::Event::kUiAction, static_cast<uint32_t>(semantic));
  }
}
void resetProgress(uint16_t remaining_s) { reset_remaining = remaining_s; }
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
  }
  if (model.scroll_reset_s > 0 &&
      now - scroll_at >= model.scroll_reset_s * 1000)
    scroll_down = true;
  time_t utc = 0;
  BindContext context{0, now, timekeeping::utcNow(utc), network::ready()};
  context.utc = utc;
  football::read(bind, &context);
  if (display_kickoff.valid() && !view.minute[0]) {
    std::tm local{};
    timekeeping::toLocal(display_kickoff.value, local);
    strftime(view.minute, sizeof(view.minute), "%d.%m. %H:%M", &local);
  }
  std::tm local{};
  view.night = model.night_enabled && timekeeping::localNow(local) &&
               timekeeping::contains(
                   {model.night_start, model.night_end},
                   static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min));
  const bool crest = selected == cfg::Screen::kCrest;
  uint8_t image = crest ? 0 : 6 + static_cast<unsigned>(selected);
  if (crest) {
    uint8_t slides[5], count = 0;
    for (uint8_t i = 1; i <= 5; ++i)
      if (images::info(i).uploaded) slides[count++] = i;
    if (count) image = slides[(now / (model.slideshow_s * 1000)) % count];
  }
  if (crest || (model.backgrounds && view.style.background))
    images::source(image, view.image, sizeof(view.image));
  auto net = network::status();
  if (net.epoch != network_epoch) {
    network_epoch = net.epoch;
    if (net.connected) boot_at = now;
  }
  view.badge_bottom = model.ip_badge_bottom;
  if ((net.ap && model.ip_badge_ap_permanent) || model.ip_badge_s == 0 ||
      now - boot_at < model.ip_badge_s * 1000) {
    if (net.ap)
      snprintf(view.badge, sizeof(view.badge), "%s\n%s", net.ap_ssid,
               net.ap_ip);
    else if (net.connected)
      snprintf(view.badge, sizeof(view.badge), "%s", net.ip);
  }
  if (reset_remaining) {
    snprintf(view.badge, sizeof(view.badge), "%s\n%u s",
             translated("Einstellungen zurücksetzen", "Reset settings"),
             reset_remaining);
    view.badge_bottom = false;
  }
  display::setBrightness(view.night ? model.night_brightness
                                    : model.brightness);
  if (!display::lock(cfg::kStatusLockTimeoutMs)) return;
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
}
bool healthy() { return good; }
}  // namespace ui
