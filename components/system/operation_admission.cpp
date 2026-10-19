#include "operation_admission.h"

#include <atomic>

namespace operations {
namespace {
std::atomic_flag active = ATOMIC_FLAG_INIT;
}
Guard::Guard() noexcept
    : held_(!active.test_and_set(std::memory_order_acquire)) {}
Guard::~Guard() {
  if (held_) active.clear(std::memory_order_release);
}
}  // namespace operations
