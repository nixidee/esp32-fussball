#include "health_meter.h"

#include <bit>

namespace health {

Coordinator::State Coordinator::stateOf(uint32_t token) noexcept {
  return static_cast<State>(token & kStateMask);
}

uint32_t Coordinator::nextToken(uint32_t token, State state) noexcept {
  return ((token + kGenerationStep) & ~kStateMask) |
         static_cast<uint32_t>(state);
}

void Coordinator::publish(State state) noexcept {
  const uint32_t before = token_.load(std::memory_order_relaxed);
  token_.store(nextToken(before, state), std::memory_order_release);
}

void Coordinator::setIntervalStart(int64_t value) noexcept {
  const uint64_t bits = std::bit_cast<uint64_t>(value);
  interval_start_low_.store(static_cast<uint32_t>(bits),
                            std::memory_order_release);
  interval_start_high_.store(static_cast<uint32_t>(bits >> 32),
                             std::memory_order_release);
}

int64_t Coordinator::intervalStart() const noexcept {
  // If a reader sees metadata from a newer owner, that owner's preceding
  // starting token happens before the final token check in observe().
  const uint64_t low = interval_start_low_.load(std::memory_order_acquire);
  const uint64_t high = interval_start_high_.load(std::memory_order_acquire);
  return std::bit_cast<int64_t>(low | (high << 32));
}

Observation Coordinator::observe() noexcept {
  const uint32_t before = token_.load(std::memory_order_acquire);
  const int64_t interval_start = intervalStart();
  Observation observation{.point = backend_.sample(backend_.context)};
  const uint32_t after = token_.load(std::memory_order_acquire);
  if (before == after) {
    switch (stateOf(before)) {
      case State::kIdle:
        observation.scope = MinimumScope::kLifetime;
        return observation;
      case State::kActive:
        observation.scope = MinimumScope::kInterval;
        observation.interval_start_ms = interval_start;
        return observation;
      default: break;
    }
  }
  observation.point.minimum_bytes = 0;
  return observation;
}

Meter::Meter(Coordinator& coordinator) noexcept : coordinator_(coordinator) {
  const Backend& backend = coordinator_.backend_;
  result_.begin = backend.sample(backend.context);
  result_.begin.minimum_bytes = 0;

  uint32_t before = coordinator_.token_.load(std::memory_order_acquire);
  if (Coordinator::stateOf(before) != Coordinator::State::kIdle) return;
  const uint32_t starting =
      Coordinator::nextToken(before, Coordinator::State::kStarting);
  if (!coordinator_.token_.compare_exchange_strong(before, starting,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
    return;
  }

  result_.start_error = backend.start(backend.context);
  if (result_.start_error != 0) {
    coordinator_.publish(Coordinator::State::kIdle);
    return;
  }

  const Point started = backend.sample(backend.context);
  result_.interval_start_ms = started.monotonic_ms;
  coordinator_.setIntervalStart(started.monotonic_ms);
  result_.owned = true;
  active_ = true;
  coordinator_.publish(Coordinator::State::kActive);
}

Meter::~Meter() { finish(); }

const MeterResult& Meter::finish() noexcept {
  if (finished_) return result_;
  finished_ = true;
  const Backend& backend = coordinator_.backend_;
  if (!active_) {
    result_.end = backend.sample(backend.context);
    result_.end.minimum_bytes = 0;
    return result_;
  }

  coordinator_.publish(Coordinator::State::kStopping);
  result_.end = backend.sample(backend.context);
  result_.stop_error = backend.stop(backend.context);
  active_ = false;
  if (result_.stop_error == 0) {
    result_.scope = MinimumScope::kInterval;
    coordinator_.publish(Coordinator::State::kIdle);
  } else {
    result_.end.minimum_bytes = 0;
    coordinator_.publish(Coordinator::State::kQuarantined);
  }
  return result_;
}

}  // namespace health
