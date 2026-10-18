// The production function is included unchanged. The real S3 profile supplies
// the backlight capability; second-owner wiring is only a synthetic API input.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include "app_config.h"
#include "targets/waveshare_esp32s3_lcd128.h"

constexpr int LEDC_LOW_SPEED_MODE = 0, LEDC_TIMER_8_BIT = 8, LEDC_TIMER_0 = 0,
              LEDC_AUTO_CLK = 0, LEDC_CHANNEL_0 = 0;
struct ledc_timer_config_t {
  int speed_mode = 0, duty_resolution = 0, timer_num = 0;
  uint32_t freq_hz = 0;
  int clk_cfg = 0;
};
struct ledc_channel_config_t {
  int gpio_num = 0, speed_mode = 0, channel = 0, timer_sel = 0;
  struct {
    bool output_invert = false;
  } flags;
};
enum class Failure { none, timer, channel, set, update };
Failure fault = Failure::none;
unsigned timers = 0, channels = 0, sets = 0, updates = 0;
uint32_t last_duty = 999;
int last_pin = hw::kNoPin;
int ledc_timer_config(const ledc_timer_config_t* value) {
  ++timers;
  assert(value->duty_resolution == 8 && value->freq_hz == cfg::kBacklightPwmHz);
  return fault == Failure::timer ? -1 : 0;
}
int ledc_channel_config(const ledc_channel_config_t* value) {
  ++channels;
  last_pin = value->gpio_num;
  return fault == Failure::channel ? -1 : 0;
}
int ledc_set_duty(int, int, uint32_t duty) {
  ++sets;
  last_duty = duty;
  return fault == Failure::set ? -1 : 0;
}
int ledc_update_duty(int, int) {
  ++updates;
  return fault == Failure::update ? -1 : 0;
}
#define ESP_ERROR_CHECK(expression)                                 \
  do {                                                              \
    if ((expression) != 0) throw std::runtime_error("SDK failure"); \
  } while (false)
namespace display {
const hw::DisplayWiring* g_wiring = nullptr;
void* g_display = nullptr;
#include "backlight_pwm.inc"
}  // namespace display
void ready() {
  display::g_display = nullptr;
  display::setBrightness(0);  // owner reset invalidates receipt
  display::g_wiring = &hw::kTarget.wiring;
  display::g_display = &timers;
  fault = Failure::none;
  timers = channels = sets = updates = 0;
  last_duty = 999;
}
int main() {
  display::setBrightness(50);
  assert(!timers && !channels && !sets && !updates);
  ready();
  display::setBrightness(0);
  assert(timers == 1 && channels == 1 && sets == 1 && updates == 1 &&
         last_duty == 0);
  display::setBrightness(0);
  assert(sets == 1 && updates == 1);
  display::setBrightness(50);
  assert(sets == 2 && updates == 2 && last_duty == 127);
  display::setBrightness(50);
  assert(sets == 2 && updates == 2);
  display::setBrightness(255);
  assert(sets == 3 && updates == 3 && last_duty == 255);
  display::setBrightness(100);
  assert(sets == 3 && updates == 3);  // same bounded duty
  display::setBrightness(0);
  assert(sets == 4 && updates == 4 && last_duty == 0);
  // A different immutable owner is configured and receives its first duty.
  auto second = hw::kTarget.wiring;
  --second.backlight;
  display::g_wiring = &second;
  display::setBrightness(0);
  assert(timers == 2 && channels == 2 && last_pin == second.backlight &&
         sets == 5 && updates == 5);
  // Failed set/update never mark the requested duty as successfully applied.
  for (const auto failure : {Failure::set, Failure::update}) {
    ready();
    display::setBrightness(10);
    const auto before_set = sets, before_update = updates;
    fault = failure;
    bool failed = false;
    try {
      display::setBrightness(80);
    } catch (const std::runtime_error&) {
      failed = true;
    }
    assert(failed && sets == before_set + 1 &&
           updates == before_update + (failure == Failure::update));
    fault = Failure::none;
    display::setBrightness(80);
    assert(sets == before_set + 2 &&
           updates == before_update + (failure == Failure::update) + 1 &&
           last_duty == 204);
    const auto done = sets;
    display::setBrightness(80);
    assert(sets == done);
  }
  // Failed initial setup retries; no cache can suppress the first zero duty.
  for (const auto failure : {Failure::timer, Failure::channel}) {
    ready();
    fault = failure;
    bool failed = false;
    try {
      display::setBrightness(0);
    } catch (const std::runtime_error&) {
      failed = true;
    }
    assert(failed && sets == 0 && updates == 0);
    fault = Failure::none;
    display::setBrightness(0);
    assert(timers == 2 && channels == (failure == Failure::channel ? 2u : 1u) &&
           sets == 1 && updates == 1 && last_duty == 0);
  }
  ready();
  display::setBrightness(0);
  display::g_display = nullptr;
  display::setBrightness(0);
  display::g_display = &timers;
  display::setBrightness(0);
  assert(timers == 2 && channels == 2 && sets == 2 && updates == 2);
  std::puts(
      "PASS actual backlight PWM: initial zero, identical duty skip, bounded "
      "values, new owner/reset and setup/set/update failure retry");
}
