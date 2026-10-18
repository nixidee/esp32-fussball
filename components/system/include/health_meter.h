// Heap phase measurements with one owner of ESP-IDF's global interval.
// Pure C++, host-tested. Only this coordinator may call the backend's
// interval start/stop; callers never access that monitor directly.

#pragma once

#include <atomic>
#include <cstdint>

namespace health {

static_assert(std::atomic<uint32_t>::is_always_lock_free);

struct Point {
  int64_t monotonic_ms = 0;
  uint32_t free_bytes = 0;
  uint32_t largest_bytes = 0;
  uint32_t minimum_bytes = 0;
};

struct Backend {
  void* context;
  Point (*sample)(void* context) noexcept;
  int32_t (*start)(void* context) noexcept;  // 0 = success
  int32_t (*stop)(void* context) noexcept;   // 0 = success
};

enum class MinimumScope : uint8_t {
  kLifetime,
  kInterval,
  kUnavailable,
};

struct Observation {
  Point point;
  MinimumScope scope = MinimumScope::kUnavailable;
  int64_t interval_start_ms = 0;
};

struct MeterResult {
  // begin.minimum_bytes is always zero. Only end.minimum_bytes has the
  // scope below; it is zero if that scope is unavailable.
  Point begin;
  Point end;
  MinimumScope scope = MinimumScope::kUnavailable;
  int64_t interval_start_ms = 0;
  int32_t start_error = 0;
  int32_t stop_error = 0;
  bool owned = false;  // successfully started an interval
};

class Meter;

class Coordinator {
 public:
  // The backend and its context outlive this coordinator and all meters.
  explicit Coordinator(Backend backend) noexcept : backend_(backend) {}
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  // Samples without waiting. A lifecycle/generation change around the
  // sample makes its minimum unavailable; free/largest/time stay valid.
  Observation observe() noexcept;

 private:
  friend class Meter;

  enum class State : uint32_t {
    kIdle,
    kStarting,
    kActive,
    kStopping,
    kQuarantined,
  };
  static constexpr uint32_t kStateMask = 7;
  static constexpr uint32_t kGenerationStep = kStateMask + 1;

  static State stateOf(uint32_t token) noexcept;
  static uint32_t nextToken(uint32_t token, State state) noexcept;
  void publish(State state) noexcept;
  void setIntervalStart(int64_t value) noexcept;
  int64_t intervalStart() const noexcept;

  Backend backend_;
  // The upper 29 bits advance on every transition. Only the owner writes
  // non-idle states; acquisition is one CAS, with no retry or spin wait.
  std::atomic<uint32_t> token_{0};
  std::atomic<uint32_t> interval_start_low_{0};
  std::atomic<uint32_t> interval_start_high_{0};
};

class Meter {
 public:
  explicit Meter(Coordinator& coordinator) noexcept;
  ~Meter();
  Meter(const Meter&) = delete;
  Meter& operator=(const Meter&) = delete;
  Meter(Meter&&) = delete;
  Meter& operator=(Meter&&) = delete;

  // Idempotent. The owner samples the interval minimum before stop and
  // releases ownership only after a successful stop. Stop failure keeps
  // the coordinator quarantined; later meters remain point-only.
  const MeterResult& finish() noexcept;

 private:
  Coordinator& coordinator_;
  MeterResult result_;
  bool active_ = false;
  bool finished_ = false;
};

}  // namespace health
