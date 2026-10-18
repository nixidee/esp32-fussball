// Whole production HTTP/submit/poll bodies are generated unchanged by the
// runner. The queue is an SDK seam; presentation callbacks do no rendering.
#include <cassert>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "command_json.h"
#include "presenter.h"
#include "ui_controller.h"

namespace probe {
uint32_t revision = 12;
unsigned generation_reads = 0;
bool race_at_submit = false, demo = true, publication_ready = true;
std::vector<ui::Action> applied;
int64_t now = 0;
settings::UiConfig desired;
}  // namespace probe
namespace settings {
uint32_t generation() {
  if (probe::race_at_submit && probe::generation_reads++ == 1)
    ++probe::revision;
  return probe::revision;
}
void uiSettings(UiConfig& out, screen_policy::Phase phase) {
  out = probe::desired;
  out.revision = probe::revision;
  out.phase = phase;
}
#include "display_same_navigation.inc"
}  // namespace settings
namespace timekeeping {
int64_t monotonicMs() { return probe::now; }
bool utcNow(time_t&) { return false; }
}  // namespace timekeeping
namespace network {
Status status() { return {}; }
bool ready() { return false; }
}  // namespace network
namespace images {
void clearSelection() {}
}  // namespace images
namespace football {
void read(Reader, void*) {}  // queue tests stop before presentation projection
DemoStatus demoStatus() {
  DemoStatus status{};
  status.enabled = probe::demo;
  return status;
}
namespace names {
esp_err_t resolveVisible(Targets&, uint8_t, uint32_t, VisibleCache&) {
  return ESP_OK;
}
}  // namespace names
}  // namespace football
namespace ui {
settings::UiConfig configuration;
const auto& model = configuration.device;
const auto& policy = configuration.screens;
const auto& generation = configuration.revision;
NavigationState nav;
ViewModel view;
PresentationState presentation;
football::names::VisibleCache visible_names;
int64_t render_at = 0, scroll_at = 0, frame_ip_at = 0, boot_at = 0;
uint32_t network_epoch = 0;
std::size_t input_count = 0;
const char* firmware_version = "test";
struct Command {
  Action action;
  uint32_t generation;
};
std::deque<Command> queue;
void* actions = &queue;
constexpr int pdPASS = 1;
int xQueueSend(void*, const Command* value, int) {
  if (queue.size() == cfg::kUiActionSlots) return 0;
  queue.push_back(*value);
  return pdPASS;
}
int xQueuePeek(void*, Command* out, int) {
  if (queue.empty()) return 0;
  *out = queue.front();
  return pdPASS;
}
int xQueueReceive(void*, Command* out, int) {
  if (queue.empty()) return 0;
  *out = queue.front();
  queue.pop_front();
  return pdPASS;
}
std::array<InputPolicy, 2> buttons;
struct ActionContext {
  Action action;
  int64_t now;
  uint32_t generation;
  bool synchronize_only;
  bool applied = false, phase_changed = false;
  settings::screen_policy::Phase required_phase =
      settings::screen_policy::Phase::kIdle;
};
void navigate(const football::Published&, void*) {}
#include "display_reload.inc"
void confirmPresentation() {}
bool action(Action value, int64_t) {
  if (!probe::publication_ready) return false;
  probe::applied.push_back(value);
  return true;
}
void bind(const football::Published&, void*) {}
images::Status imageSource(uint8_t, cfg::StockImage) { return {}; }
void Presenter::finishStatus(uint32_t, int64_t, const images::Status&) {}
bool draw(uint32_t, const images::Status&) { return true; }
#define ESP_LOGW(...) ((void)0)
#include "display_input.inc"
#include "display_poll.inc"
#undef ESP_LOGW
#include "display_submit.inc"
}  // namespace ui

struct httpd_req_t {
  std::string body, code, field;
  std::size_t at = 0;
  bool authorized = true;
  explicit httpd_req_t(std::string bytes) : body(std::move(bytes)) {}
};
struct Request {
  explicit Request(httpd_req_t*) {}
};
struct Fault {
  const char* code;
  int status;
};
namespace fault {
constexpr Fault kBusy{"busy", 409}, kSettingsChanged{"settings_changed", 409},
    kJson{"json", 400}, kBodyIncomplete{"body_incomplete", 400};
}
const Fault* admitSession(httpd_req_t* request) {
  static constexpr Fault denied{"session", 401};
  return request->authorized ? nullptr : &denied;
}
const Fault* checkJsonBody(httpd_req_t*) { return nullptr; }
struct JsonBody {
  httpd_req_t* request;
  explicit JsonBody(httpd_req_t* request) : request(request) {}
  json::Input input() {
    return {
        request, [](void* raw) {
          auto& request = *static_cast<httpd_req_t*>(raw);
          return request.at < request.body.size()
                     ? static_cast<unsigned char>(request.body[request.at++])
                     : -1;
        }};
  }
  bool complete() const { return request->at == request->body.size(); }
  const json::ErrorInfo* diagnostic() const { return nullptr; }
};
bool body_broken = false;
int error(httpd_req_t* request, const Fault& fault, const char* = nullptr,
          const json::ErrorInfo* = nullptr) {
  request->code = fault.code;
  return fault.status;
}
int error(httpd_req_t* request, const char*, const char* code, const char*) {
  request->code = code;
  return 409;
}
int rejected(httpd_req_t* request, const settings::Violation& why) {
  request->code = "violation";
  request->field = why.path.data();
  return 400;
}
int failed(httpd_req_t* request, esp_err_t) {
  request->code = "state";
  return 409;
}
int success(httpd_req_t*) { return 200; }
using namespace web;
#include "command_body.inc"
#include "display_http.inc"

std::string body(const char* action, uint32_t revision) {
  return "{\"action\":\"" + std::string(action) +
         "\",\"config_revision\":" + std::to_string(revision) + "}";
}
void reset() {
  probe::revision = 12;
  probe::generation_reads = 0;
  probe::race_at_submit = false;
  probe::demo = probe::publication_ready = true;
  probe::applied.clear();
  ui::queue.clear();
  ui::actions = &ui::queue;
  probe::now = 0;
  probe::desired = {};
  probe::desired.revision = 12;
  probe::desired.device.input_roles[0] = cfg::InputRole::kMandatory;
  probe::desired.device.scroll_repeat_ms = cfg::kScrollRepeatDefaultMs;
  ui::configuration = probe::desired;
  ui::buttons = {};
  ui::input_count = 1;
  ui::nav = {};
  ui::nav.initialized = ui::nav.manual = true;
  ui::nav.page = 2;
  ui::render_at = 0;
}
void sample(bool active, int64_t now) {
  probe::now = now;
  ui::input(0, active);
}
void cosmeticApply(int64_t now, bool active) {
  ++probe::revision;
  probe::desired.screens.appearance[0].accent ^= 0x123456;
  sample(active, now);
  assert(ui::nav.initialized && ui::nav.manual && ui::nav.page == 2);
}
void inputRevisionCases() {
  // Mandatory click was released, but its double-click window is still open.
  reset();
  sample(true, 0);
  sample(true, 40);
  sample(false, 100);
  sample(false, 140);
  assert(ui::queue.empty());
  cosmeticApply(200, false);
  sample(false, 441);
  assert(ui::queue.empty());  // old pending Short must not acquire new revision
  sample(true, 500);
  sample(true, 540);
  sample(false, 600);
  sample(false, 640);
  sample(false, 941);
  assert(ui::queue.size() == 1 &&
         ui::queue.front().action == ui::Action::kNext &&
         ui::queue.front().generation == 13);
  // The old second press must not become a Double after Apply either.
  reset();
  sample(true, 0);
  sample(true, 40);
  sample(false, 100);
  sample(false, 140);
  sample(true, 200);
  sample(true, 240);
  cosmeticApply(260, true);
  sample(false, 300);
  sample(false, 340);
  sample(false, 700);
  assert(ui::queue.empty());
  // A held Mandatory key stays silent through its old long deadline and
  // release.
  reset();
  sample(true, 0);
  sample(true, 40);
  cosmeticApply(300, true);
  sample(true, 1000);
  sample(false, 1040);
  sample(false, 1080);
  assert(ui::queue.empty());
  sample(true, 1120);
  sample(true, 1160);
  sample(true, 1761);
  assert(ui::queue.size() == 1 &&
         ui::queue.front().action == ui::Action::kDefault &&
         ui::queue.front().generation == 13);
  // Repeating Up resumes only after release and a genuinely new press.
  reset();
  probe::desired.device.input_roles[0] = cfg::InputRole::kUp;
  ui::configuration = probe::desired;
  sample(true, 0);
  sample(true, 40);
  sample(true, 641);
  assert(ui::queue.size() == 1);
  ui::queue.clear();
  cosmeticApply(650, true);
  sample(true, 1000);
  sample(true, 2000);
  sample(false, 2040);
  sample(false, 2080);
  assert(ui::queue.empty());
  sample(true, 2120);
  sample(true, 2160);
  sample(true, 2761);
  sample(true, 3012);
  assert(ui::queue.size() == 2);
  for (const auto& command : ui::queue)
    assert(command.action == ui::Action::kUp && command.generation == 13);
  // Same-revision reload (e.g. phase projection) does not cancel a valid click.
  reset();
  sample(true, 0);
  sample(true, 40);
  sample(false, 100);
  sample(false, 140);
  ui::reloadSettings(200, ui::nav.phase);
  sample(false, 441);
  assert(ui::queue.size() == 1 &&
         ui::queue.front().action == ui::Action::kNext &&
         ui::queue.front().generation == 12);
}
int main() {
  reset();
  for (const char* action : {"next", "ip", "auto_next"}) {
    httpd_req_t request{body(action, 12)};
    assert(postDisplay(&request) == 200 && ui::queue.back().generation == 12);
  }
  ui::poll();
  assert(ui::queue.empty() &&
         probe::applied ==
             std::vector<ui::Action>({ui::Action::kNext, ui::Action::kShowIp,
                                      ui::Action::kAutomaticNext}));
  // Stale-at-HTTP commands are never relabelled to the server's revision.
  reset();
  for (const char* action : {"next", "ip", "auto_next"}) {
    httpd_req_t request{body(action, 11)};
    assert(postDisplay(&request) == 409 && request.code == "settings_changed");
    assert(ui::queue.empty() && probe::applied.empty());
  }
  // A save after HTTP's first check is still rejected by actual submit().
  reset();
  probe::race_at_submit = true;
  httpd_req_t race{body("next", 12)};
  assert(postDisplay(&race) == 409 && race.code == "settings_changed" &&
         ui::queue.empty());
  // Already queued old revisions are discarded by the actual complete poll.
  reset();
  httpd_req_t old{body("next", 12)};
  assert(postDisplay(&old) == 200);
  ++probe::revision;
  httpd_req_t fresh{body("ip", 13)};
  assert(postDisplay(&fresh) == 200);
  ui::poll();
  assert(ui::queue.empty() &&
         probe::applied == std::vector<ui::Action>{ui::Action::kShowIp});
  // Current commands remain FIFO while their matching data publication waits.
  reset();
  probe::publication_ready = false;
  httpd_req_t one{body("next", 12)}, two{body("ip", 12)};
  assert(postDisplay(&one) == 200 && postDisplay(&two) == 200);
  ui::poll();
  assert(ui::queue.size() == 2 && probe::applied.empty());
  probe::publication_ready = true;
  ui::poll();
  assert(ui::queue.empty() &&
         probe::applied ==
             std::vector<ui::Action>({ui::Action::kNext, ui::Action::kShowIp}));
  reset();
  for (unsigned i = 0; i < cfg::kUiActionSlots; ++i) {
    httpd_req_t request{body("next", 12)};
    assert(postDisplay(&request) == 200);
  }
  httpd_req_t full{body("ip", 12)};
  assert(postDisplay(&full) == 409 && full.code == "busy" &&
         ui::queue.size() == cfg::kUiActionSlots);
  reset();
  probe::demo = false;
  httpd_req_t demo{body("auto_next", 12)};
  assert(postDisplay(&demo) == 409 && demo.code == "demo_disabled" &&
         ui::queue.empty());
  httpd_req_t next{body("next", 12)};
  assert(postDisplay(&next) == 200);  // ordinary action unaffected
  reset();
  for (const char* text :
       {"{}", "{\"action\":\"next\"}",
        "{\"action\":\"next\",\"config_revision\":-1}",
        "{\"action\":\"next\",\"config_revision\":4294967296}",
        "{\"action\":\"next\",\"config_revision\":12} trailing"}) {
    httpd_req_t request{text};
    assert(postDisplay(&request) == 400 && ui::queue.empty());
  }
  httpd_req_t unauthorized{body("next", 12)};
  unauthorized.authorized = false;
  assert(postDisplay(&unauthorized) == 401 && ui::queue.empty());
  inputRevisionCases();
  std::puts(
      "PASS actual display decoder/HTTP/submit/poll: caller revision, stale "
      "HTTP, save race, queued old discard, waiting FIFO, full queue, Demo and "
      "malformed/admission failures; actual input/reload/InputPolicy pending, "
      "double, held/repeat cancellation, fresh release and unchanged revision");
}
