// Exclusive admission for operations whose TLS/parser/file/decoder working
// sets must not overlap. No waiting, queue, task or heap allocation.
#pragma once

namespace operations {
class Guard {
 public:
  Guard() noexcept;
  ~Guard();
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
  explicit operator bool() const { return held_; }

 private:
  bool held_;
};
}  // namespace operations
