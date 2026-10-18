// Fixed state for the real normal-flush lifecycle. The display port serializes
// task/ISR access; this class owns no LVGL objects, buffers or allocations.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "display_port.h"

namespace display {
namespace detail {
class SceneProgress {
 public:
  bool accept(uint32_t token) noexcept {
    if (requested_valid_ && requested_ == token) return false;
    requested_ = token;
    requested_valid_ = true;
    return true;
  }
  // Called before the driver, since the ISR may run before draw_bitmap returns.
  bool begin(bool last) noexcept {
    if (submitting_ || size_ == stripes_.size()) return false;
    if (!in_pass_) {
      pass_ = requested_;
      pass_valid_ = requested_valid_;
      pass_failed_ = false;
      in_pass_ = true;
    }
    current_ = (head_ + size_) % stripes_.size();
    stripes_[current_] = {pass_,       0,     last, pass_failed_,
                          pass_valid_, false, false};
    ++size_;
    submitting_ = true;
    if (last) in_pass_ = false;
    return true;
  }
  void submitted() noexcept {
    if (!submitting_) return;
    stripes_[current_].submitted = true;
    submitting_ = false;
    collect();
  }
  // The driver has drained the queue and proved this buffer is free. A failed
  // pass can never acknowledge a scene, even when earlier stripes completed.
  void failed() noexcept {
    if (!submitting_) return;
    auto& stripe = stripes_[current_];
    stripe.failed = stripe.complete = stripe.submitted = true;
    submitting_ = false;
    if (in_pass_) pass_failed_ = true;
    collect();
  }
  void completed(int64_t at_ms) noexcept {
    for (std::size_t i = 0; i < size_; ++i) {
      auto& stripe = stripes_[(head_ + i) % stripes_.size()];
      if (!stripe.complete) {
        stripe.complete = true;
        stripe.at_ms = at_ms;
        break;
      }
    }
    collect();
  }
  SceneCompletion completion() const noexcept { return completion_; }

 private:
  struct Stripe {
    uint32_t scene;
    int64_t at_ms;
    bool last, failed, valid, submitted, complete;
  };
  void collect() noexcept {
    while (size_) {
      const auto& stripe = stripes_[head_];
      if (!stripe.submitted || !stripe.complete) break;
      if (stripe.last && stripe.valid && !stripe.failed && requested_valid_ &&
          stripe.scene == requested_)
        completion_ = {stripe.scene, stripe.at_ms, true};
      head_ = (head_ + 1) % stripes_.size();
      --size_;
    }
  }
  std::array<Stripe, 2> stripes_{};
  SceneCompletion completion_;
  uint32_t requested_ = 0, pass_ = 0;
  uint8_t head_ = 0, size_ = 0, current_ = 0;
  bool requested_valid_ = false, pass_valid_ = false, pass_failed_ = false,
       in_pass_ = false, submitting_ = false;
};
static_assert(sizeof(SceneProgress) <= 112);
}  // namespace detail
}  // namespace display
