#include "ui_controller.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <new>

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
#include "presenter.h"
#include "settings_store.h"
#include "time_service.h"
namespace ui {
namespace {
geometry::SafeArea area(0, 0, hw::DisplayShape::kRect, 0);
settings::Model model;
settings::screen_policy::Policy policy;
ViewModel view;
NavigationState nav;
PresentationState presentation;
int64_t render_at = 0, boot_at = 0, frame_ip_at = 0, scroll_at = 0;
bool good = false, dim_colours = false;
std::size_t input_count = 0;
uint32_t generation = 0, data_generation = 0, image_revision = 0,
         network_epoch = 0;
hw::DisplayProfile profile{};
std::atomic<uint32_t> screen_state{
    1u << 16 |
    static_cast<uint32_t>(settings::screen_policy::Phase::kIdle) << 19};
static_assert(settings::screen_policy::kPhaseCount <= 8 &&
              2 + screen::kSportTypeCount * cfg::kCompetitionMax < 64);
std::array<InputPolicy, hw::kMaxInputs> buttons{};
StaticQueue_t action_buffer;
events::Payload action_storage[cfg::kEventUiActionSlots];
QueueHandle_t actions = nullptr;
void onAction(events::Event, events::Payload action, void*) {
  if (xQueueSend(actions, &action, 0) != pdPASS)
    ESP_LOGW("input", "pending action dropped");
}
images::Status imageSource(uint8_t id, cfg::StockImage stock) {
  return images::select(id, stock, view.image, sizeof(view.image));
}
NavigationData factsOf(const football::Published& data,
                       const BindContext& context) {
  uint8_t failed = 0;
  for (std::size_t i = 0; i < data.routes.entries.size(); ++i) {
    const auto* state =
        football::findState(data.competitions, data.routes.entries[i].id);
    if (state && state->cause != football::Cause::kNone) failed |= 1u << i;
  }
  return {data.store,
          data.shown,
          data.routes,
          model.demo
              ? data.clock
              : football::Clock{context.valid ? context.utc : 0,
                                static_cast<uint32_t>(context.now / 1000)},
          {[](int64_t at, void*) { return timekeeping::localOffset(at); },
           nullptr, 0},
          data.loaded_matches,
          data.loaded_tables,
          failed,
          data.configured,
          data.break_proven};
}
void bind(const football::Published& data, void* raw) {
  auto& context = *static_cast<BindContext*>(raw);
  if (data.generation != data_generation ||
      generation != settings::generation())
    return;
  if (model.demo) {
    context.utc = data.clock.utc;
    context.valid = data.clock.utc > 0;
  }
  const auto facts = factsOf(data, context);
  NavigationContext navigation(policy, facts);
  const auto before = nav.entry;
  const auto page_before = nav.page;
  sequence::synchronize(nav, navigation.input(), context.now);
  Presenter presenter(model, view, policy, nav, presentation, input_count,
                      &data, &navigation);
  context.status_state = presenter.bind(context);
  context.bound = true;
  const auto automatic_entries = model.demo && !nav.frame
                                     ? sequence::count(navigation.input(), false)
                                     : 0;
  screen_state.store(presentation.screen_status |
                         static_cast<uint32_t>(presentation.starting) << 16 |
                         static_cast<uint32_t>(nav.manual) << 17 |
                         static_cast<uint32_t>(nav.frame) << 18 |
                         static_cast<uint32_t>(nav.phase) << 19 |
                         static_cast<uint32_t>(automatic_entries) << 22,
                     std::memory_order_relaxed);
  if (model.demo && (before != nav.entry || page_before != nav.page))
    ESP_LOGI("screen", "demo phase %u type %u competition %u page %u/%u %s%s",
             static_cast<unsigned>(nav.phase),
             static_cast<unsigned>(nav.entry.type), nav.entry.competition,
             nav.page + 1, nav.pages, nav.manual ? "manual" : "automatic",
             nav.frame ? " frame" : "");
}
struct ActionContext {
  Action action;
  int64_t now;
  uint32_t generation;
  bool synchronize_only;
  bool applied = false;
};
void navigate(const football::Published& data, void* raw) {
  auto& action = *static_cast<ActionContext*>(raw);
  // A save can complete while this callback waits for the repository lock.
  // Reject an old command before any navigation, start-screen or timer effect.
  if (action.generation != settings::generation()) return;
  time_t utc = 0;
  const bool valid = timekeeping::utcNow(utc);
  const BindContext context{utc, action.now, valid, false, false, nullptr};
  if (data.generation != data_generation) return;
  const auto facts = factsOf(data, context);
  NavigationContext navigation(policy, facts);
  if (action.generation != settings::generation()) return;
  if (action.action == Action::kAutomaticNext && !model.demo) {
    // Even a forged/previously queued command cannot affect ordinary output.
    action.applied = true;
    return;
  }
  if (action.synchronize_only)
    sequence::reset(nav, navigation.input(), action.now);
  else {
    sequence::applyAction(nav, navigation.input(), action.action, action.now);
    if (model.demo)
      ESP_LOGI("screen", "demo action %u phase %u type %u competition %u "
                         "page %u/%u %s%s",
               static_cast<unsigned>(action.action),
               static_cast<unsigned>(nav.phase),
               static_cast<unsigned>(nav.entry.type), nav.entry.competition,
               nav.page + 1, nav.pages, nav.manual ? "manual" : "automatic",
               nav.frame ? " frame" : "");
    presentation.starting = false;
    if (action.action == Action::kShowIp) boot_at = frame_ip_at = action.now;
    if (action.action == Action::kToggleDirection ||
        action.action == Action::kUp || action.action == Action::kDown)
      scroll_at = action.now;
  }
  action.applied = true;
}
bool action(Action input, int64_t now) {
  ActionContext context{input, now, generation, false};
  football::read(navigate, &context);
  return context.applied;
}
void reloadSettings(int64_t now) {
  settings::uiSettings(model, policy, generation, data_generation);
  for (auto& button : buttons) button.cancel();
  nav.initialized = false;
  scroll_at = now;
  nav.scroll_down = true;
  ActionContext context{Action::kNone, now, generation, true};
  football::read(navigate, &context);
}
bool draw(uint32_t lock_timeout_ms) {
  display::setBrightness(view.night ? model.night_brightness
                                    : model.brightness);
  if (!display::lock(lock_timeout_ms)) return false;
  display::setRotation(model.rotation);
  if (image_revision != images::revision()) {
    image_revision = images::revision();
    display::dropImageCache(view.image);
  }
  auto rotated = profile;
  if (model.rotation % 2) std::swap(rotated.width, rotated.height);
  area = geometry::SafeArea::forDisplay(rotated, cfg::kContentMarginDivisor);
  good = render(view, area);
  display::unlock();
  return good;
}
}  // namespace
esp_err_t init(const hw::DisplayProfile& display, std::size_t inputs,
               bool backlight) {
  dim_colours = !backlight;
  area = geometry::SafeArea::forDisplay(display, cfg::kContentMarginDivisor);
  profile = display;
  input_count = std::min(inputs, buttons.size());
  boot_at = frame_ip_at = timekeeping::monotonicMs();
  if (!area.isValid()) return ESP_ERR_INVALID_ARG;
  actions = xQueueCreateStatic(
      cfg::kEventUiActionSlots, sizeof(events::Payload),
      reinterpret_cast<uint8_t*>(action_storage), &action_buffer);
  if (!actions) return ESP_ERR_NO_MEM;
  reloadSettings(boot_at);
  return events::subscribe(events::Event::kUiAction, onAction, nullptr);
}
void input(std::size_t index, bool active) {
  if (index >= input_count) return;
  const int64_t now = timekeeping::monotonicMs();
  if (generation != settings::generation()) reloadSettings(now);
  if (generation != settings::generation()) return;
  const auto role = model.input_roles[index];
  // Only the mandatory input waits for a second press (double = Previous).
  const auto emitted = buttons[index].update(
      active, now, role == cfg::InputRole::kMandatory, model.scroll_repeat_ms);
  for (unsigned event = 0; event <= static_cast<unsigned>(RawInput::kRepeat);
       ++event) {
    if (!(emitted & (1u << event))) continue;
    const auto semantic =
        mapInput(role, static_cast<RawInput>(event), nav.scroll_down);
    if (semantic != Action::kNone)
      events::post(events::Event::kUiAction,
                   actionPayload(semantic, generation));
  }
}
void poll() {
  const int64_t now = timekeeping::monotonicMs();
  events::Payload pending;
  // A successful settings/competition save starts a complete automatic state.
  // Commands retain their producer's model version through both queues.
  // Provider publications do not change this settings generation.
  if (generation != settings::generation()) reloadSettings(now);
  for (std::size_t i = 0; actions && i < cfg::kEventUiActionSlots; ++i) {
    if (xQueuePeek(actions, &pending, 0) != pdPASS) break;
    // A newer command may already be queued while our model is still old.
    // Keep it in the FIFO until the next poll adopts the newer snapshot.
    if (generation != settings::generation()) return;
    if (actionGeneration(pending) == generation) {
      const auto input = actionFromPayload(pending);
      // A newly saved provider/source snapshot may not have reached the
      // repository yet. Keep this fresh command at the FIFO head until the
      // matching publication exists; old-generation commands are discarded.
      if (input != Action::kNone && !action(input, now)) return;
    }
    if (xQueueReceive(actions, &pending, 0) != pdPASS) break;
  }
  // Also catch a save during reload or the final navigation callback.
  if (generation != settings::generation()) return;
  if (now - render_at < cfg::kUiRefreshMs) return;
  render_at = now;
  if (model.scroll_reset_s > 0 &&
      now - scroll_at >= model.scroll_reset_s * 1000LL)
    nav.scroll_down = true;
  time_t utc = 0;
  const auto net = network::status();
  BindContext context{0,      now, timekeeping::utcNow(utc), network::ready(),
                      net.ap, &net};
  context.utc = utc;
  football::read(bind, &context);
  if (!context.bound || generation != settings::generation()) return;
  std::tm local{};
  view.night = model.night_enabled && timekeeping::localNow(local) &&
               timekeeping::contains(
                   {model.night_start, model.night_end},
                   static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min));
  // Night brightness as an amount of light: colour value = level^(1/2.2).
  if (view.night && dim_colours)
    view.night_colour = static_cast<uint8_t>(std::lround(
        255.0f * std::pow(model.night_brightness / 100.0f, 1.0f / 2.2f)));
  images::Status selected_image{};
  if (presentation.starting) {
    images::clearSelection();
  } else if (nav.entry.valid() && !screen::sporting(nav.entry.type)) {
    selected_image = imageSource(nav.image_id, cfg::kStockCrest);
  } else if (policy.backgrounds && view.style.background) {
    selected_image = imageSource(view.style.image, cfg::kStockBackground);
  } else
    images::clearSelection();
  view.image_revision = selected_image.attempt;
  if (!presentation.starting)
    Presenter(model, view, policy, nav, presentation, input_count)
        .finishStatus(context.status_state, now, selected_image);
  if (net.epoch != network_epoch) {
    network_epoch = net.epoch;
    if (net.connected && !policy.frame) boot_at = now;
  }
  view.badge_bottom = model.ip_badge_bottom;
  if (!presentation.no_badge &&
      ((net.ap && model.ip_badge_ap_permanent && !policy.frame) ||
       model.ip_badge_s == 0 ||
       now - (policy.frame ? frame_ip_at : boot_at) <
           model.ip_badge_s * 1000)) {
    if (net.ap) {
      snprintf(view.badge[0], sizeof(view.badge[0]), "%s", net.ap_ssid);
      snprintf(view.badge[1], sizeof(view.badge[1]), "%s", net.ap_ip);
      view.badge_lines = 2;
    } else if (net.connected) {
      snprintf(view.badge[0], sizeof(view.badge[0]), "%s", net.ip);
      view.badge_lines = 1;
    }
  }
  const uint32_t revision = nav.revision;
  const bool image_missing = !screen::sporting(nav.entry.type) &&
                             selected_image.active &&
                             selected_image.actual == images::FileSource::kNone;
  if (draw(cfg::kStatusLockTimeoutMs) && !presentation.starting &&
      generation == settings::generation()) {
    if (image_missing) {
      // An image-error card is useful output, but earns no image time and
      // cannot consume the ordinary slideshow's continuation cursor.
      nav.presented = false;
      nav.observed_at = timekeeping::monotonicMs();
    } else
      sequence::presented(nav, revision, timekeeping::monotonicMs());
  }
}
bool showStartScreen(const hw::DisplayProfile& display, bool backlight) {
  dim_colours = !backlight;
  profile = display;
  settings::uiSettings(model, policy, generation, data_generation);
  new (&view) ViewModel();
  Presenter(model, view, policy, nav, presentation, input_count).bindStart(0);
  return draw(cfg::kBootScreenLockTimeoutMs) && good;
}
bool healthy() { return good; }
Status status() {
  const uint32_t word = screen_state.load(std::memory_order_relaxed);
  return {static_cast<screen::Type>(word & 0xff),
          static_cast<Reason>(word >> 8 & 0xff), (word & (1u << 16)) != 0,
          (word & (1u << 17)) != 0, (word & (1u << 18)) != 0,
          static_cast<uint8_t>(word >> 19 & 7),
          static_cast<uint8_t>(word >> 22 & 63)};
}
const char* reasonName(Reason reason) {
  switch (reason) {
    case Reason::kSetup: return "setup";
    case Reason::kManual: return "manual";
    case Reason::kOwnMatch: return "own_match";
    case Reason::kMatchday: return "matchday";
    case Reason::kIdle: return "idle";
    case Reason::kOtherGames: return "other_games";
    case Reason::kBreak: return "break";
    case Reason::kFrame: return "frame";
  }
  return "idle";
}
}  // namespace ui
