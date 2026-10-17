#pragma once

#include <cerrno>
#include <cstdint>

namespace football::http {

// ESP-IDF's SSL write first polls with its supplied timeout, then returns
// the TLS write's result. A zero-timeout poll returning 0, socket EAGAIN,
// or TLS WANT is retried only by the outer bounded wait loop.
inline bool retryWrite(int result, int socket_error, int want_read,
                       int want_write) noexcept {
  return result == 0 || result == want_read || result == want_write ||
         (result == -1 &&
          (socket_error == EAGAIN || socket_error == EWOULDBLOCK));
}

// One HTTP request's first-send boundary, shared by the real transport and
// host checks. DNS/TLS can be cancelled before a send attempt. Once admitted,
// a failed/WANT write is a conservative counted attempt, and later writes
// neither reserve again nor acquire the admission lock again.
class SendAdmission {
 public:
  bool allowed(bool frame, bool pause_with_frame) noexcept {
    paused_ |= !started_ && pause_with_frame && frame;
    return !paused_;
  }

  template <class Begin, class Write, class End>
  int write(Begin begin, Write write, End end) {
    if (paused_) return -1;
    if (started_) return write();
    refusal_ = begin();
    if (refusal_ != 0) return -1;
    started_ = true;
    const int result = write();
    // The first nonblocking write always releases the transferred lock,
    // including zero/error/WANT. Never hold it for a later write/response.
    end();
    return result;
  }

  bool started() const noexcept { return started_; }
  bool paused() const noexcept { return paused_; }
  int32_t refusal() const noexcept { return refusal_; }

 private:
  int32_t refusal_ = 0;
  bool started_ = false, paused_ = false;
};

}  // namespace football::http
