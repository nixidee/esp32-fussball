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
ViewModel view;
NavigationState nav;
PresentationState presentation;
int64_t render_at = 0, boot_at = 0;
bool good = false, dim_colours = false;
std::size_t input_count = 0;
uint32_t generation = 0, image_revision = 0, network_epoch = 0;
hw::DisplayProfile profile{};
std::atomic<uint32_t> screen_state{1u << 16};
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
                       const BindContext& context,
                       uint8_t (&faults)[cfg::kCompetitionMax]) {
  std::size_t count = 0;
  for (const auto& competition : data.competitions)
    if (competition.active && competition.id &&
        competition.cause != football::Cause::kNone)
      faults[count++] = competition.id;
  return {data.store,
          data.shown,
          faults,
          count,
          context.valid ? context.utc : 0,
          0,
          data.configured};
}
void bind(const football::Published& data, void* raw) {
  auto& context = *static_cast<BindContext*>(raw);
  uint8_t faults[cfg::kCompetitionMax];
  const auto facts = factsOf(data, context, faults);
  Presenter presenter(model, view, nav, presentation, input_count, &data,
                      &facts);
  context.status_state = presenter.bind(context);
  screen_state.store(presentation.screen_status |
                         static_cast<uint32_t>(presentation.starting) << 16,
                     std::memory_order_relaxed);
}
struct ActionContext {
  Action action;
  int64_t now;
  uint32_t generation;
  bool synchronize_only;
};
void navigate(const football::Published& data, void* raw) {
  const auto& action = *static_cast<ActionContext*>(raw);
  // A save can complete while this callback waits for the repository lock.
  // Reject an old command before any navigation, start-screen or timer effect.
  if (action.generation != settings::generation()) return;
  time_t utc = 0;
  const bool valid = timekeeping::utcNow(utc);
  const BindContext context{utc, action.now, valid, false, false, nullptr};
  uint8_t faults[cfg::kCompetitionMax];
  auto facts = factsOf(data, context, faults);
  if (!action.synchronize_only && nav.selected == cfg::Screen::kMatchday &&
      valid)
    facts.local_offset = timekeeping::localOffset(utc);
  if (action.generation != settings::generation()) return;
  if (action.synchronize_only)
    synchronize(nav, model, facts, input_count);
  else {
    applyAction(nav, action.action, model, facts, action.now);
    presentation.starting = false;
    if (action.action == Action::kShowIp) boot_at = action.now;
  }
}
void action(Action input, int64_t now) {
  ActionContext context{input, now, generation, false};
  football::read(navigate, &context);
}
void reloadSettings(int64_t now) {
  model = settings::current(&generation);
  for (auto& button : buttons) button.cancel();
  automatic(nav, now, AutomaticReason::kSettings);
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
    if (xQueueReceive(actions, &pending, 0) != pdPASS) break;
    if (actionGeneration(pending) != generation) continue;
    const auto input = actionFromPayload(pending);
    if (input != Action::kNone) action(input, now);
  }
  // Also catch a save during reload or the final navigation callback.
  if (generation != settings::generation()) return;
  if (now - render_at < cfg::kUiRefreshMs) return;
  render_at = now;
  if (nav.manual && model.manual_return_s > 0 &&
      now - nav.input_at >= model.manual_return_s * 1000) {
    automatic(nav, now, AutomaticReason::kTimeout);
  }
  if (model.scroll_reset_s > 0 &&
      now - nav.scroll_at >= model.scroll_reset_s * 1000)
    nav.scroll_down = true;
  time_t utc = 0;
  const auto net = network::status();
  BindContext context{0,      now, timekeeping::utcNow(utc), network::ready(),
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
    view.night_colour = static_cast<uint8_t>(std::lround(
        255.0f * std::pow(model.night_brightness / 100.0f, 1.0f / 2.2f)));
  images::Status selected_image{};
  if (presentation.starting) {
    images::clearSelection();
  } else if (nav.selected == cfg::Screen::kCrest) {
    uint8_t id = cfg::kStockImageId;
    if (!model.slides.empty()) {
      const auto count = model.slides.size();
      const unsigned index = (now / (model.slideshow_s * 1000)) % count;
      id = model.slides[index];
      if (count > 1 && view.layout == Layout::kImage) {
        view.page = static_cast<uint8_t>(index);
        view.pages = static_cast<uint8_t>(count);
      }
    }
    selected_image = imageSource(id, cfg::kStockCrest);
  } else if (model.backgrounds && view.style.background) {
    selected_image = imageSource(view.style.image, cfg::kStockBackground);
  } else
    images::clearSelection();
  view.image_revision = selected_image.attempt;
  if (!presentation.starting)
    Presenter(model, view, nav, presentation, input_count)
        .finishStatus(context.status_state, now, selected_image);
  if (net.epoch != network_epoch) {
    network_epoch = net.epoch;
    if (net.connected) boot_at = now;
  }
  view.badge_bottom = model.ip_badge_bottom;
  if (!presentation.no_badge &&
      ((net.ap && model.ip_badge_ap_permanent) || model.ip_badge_s == 0 ||
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
  Presenter(model, view, nav, presentation, input_count).bindStart(0);
  return draw(cfg::kBootScreenLockTimeoutMs) && good;
}
bool healthy() { return good; }
Status status() {
  const uint32_t word = screen_state.load(std::memory_order_relaxed);
  return {static_cast<cfg::Screen>(word & 0xff),
          static_cast<Reason>(word >> 8 & 0xff), (word & (1u << 16)) != 0};
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
