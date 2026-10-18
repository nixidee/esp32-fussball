// Pure state; the adapter serializes access and supplies clock/waits.
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "original_names_channel.h"

namespace football::originals::detail {
static_assert(cfg::kOriginalNamesBufferBytes > 0 &&
              cfg::kOriginalNamesBufferBytes <=
                  std::numeric_limits<uint16_t>::max());
class Channel {
 public:
  Channel() = default;
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  bool begin(const settings::Source& source, uint64_t now) {
    if (query_ || serial_ == std::numeric_limits<uint32_t>::max()) return false;
    source_ = source;
    query_ = ++serial_;
    deadline_ = now + cfg::kOperationDeadlineMs;
    head_ = count_ = 0;
    claimed_ = worker_done_ = consumer_done_ = false;
    state_ = State::kPending;
    error_ = ESP_OK;
    return true;
  }
  uint32_t query() const { return query_; }
  uint64_t deadline(uint32_t query) const {
    return matches(query) ? deadline_ : 0;
  }
  bool claim(uint32_t query, settings::Source& source) {
    if (!matches(query) || claimed_ || worker_done_) return false;
    claimed_ = true;
    source = source_;
    if (state_ == State::kPending) state_ = State::kStreaming;
    return true;
  }
  bool active(uint32_t query) const {
    return matches(query) && !worker_done_ &&
           (state_ == State::kPending || state_ == State::kStreaming);
  }
  std::size_t write(uint32_t query, std::string_view bytes) {
    if (!active(query) || !claimed_) return 0;
    const auto count = std::min(bytes.size(), buffer_.size() - count_);
    const auto tail = (head_ + count_) % buffer_.size();
    const auto first = std::min(count, buffer_.size() - tail);
    if (first) std::memcpy(buffer_.data() + tail, bytes.data(), first);
    if (count > first)
      std::memcpy(buffer_.data(), bytes.data() + first, count - first);
    count_ += count;
    return count;
  }
  Read read(uint32_t query, char* output, std::size_t capacity) {
    if (!matches(query)) return {};
    const auto count = std::min(capacity, std::size_t(count_));
    const auto first = std::min(count, buffer_.size() - head_);
    if (first) std::memcpy(output, buffer_.data() + head_, first);
    if (count > first)
      std::memcpy(output + first, buffer_.data(), count - first);
    head_ = (head_ + count) % buffer_.size();
    count_ -= count;
    const auto visible =
        state_ == State::kComplete && count_ ? State::kStreaming : state_;
    return {count, visible, error_};
  }
  void expire(uint64_t now) {
    if (query_ && !worker_done_ && now >= deadline_ && active(query_)) {
      state_ = State::kFailed;
      error_ = ESP_ERR_TIMEOUT;
      count_ = 0;
    }
  }
  void complete(uint32_t query, esp_err_t error) {
    if (!matches(query) || worker_done_) return;
    worker_done_ = true;
    if (activeState()) {
      state_ = error == ESP_OK ? State::kComplete : State::kFailed;
      error_ = error;
      if (error != ESP_OK) count_ = 0;
    }
    closeIfDone();
  }
  void cancel(uint32_t query) {
    if (!matches(query)) return;
    consumer_done_ = true;
    if (activeState() || state_ == State::kComplete) {
      state_ = State::kCancelled;
      error_ = ESP_ERR_INVALID_STATE;
    }
    count_ = 0;
    closeIfDone();
  }
  void release(uint32_t query) { cancel(query); }

 private:
  std::array<char, cfg::kOriginalNamesBufferBytes> buffer_{};
  settings::Source source_{};
  uint64_t deadline_ = 0;
  uint32_t serial_ = 0, query_ = 0;
  esp_err_t error_ = ESP_OK;
  uint16_t head_ = 0, count_ = 0;
  State state_ = State::kStale;
  bool claimed_ = false, worker_done_ = false, consumer_done_ = false;
  bool matches(uint32_t query) const { return query && query == query_; }
  bool activeState() const {
    return state_ == State::kPending || state_ == State::kStreaming;
  }
  void closeIfDone() {
    if (worker_done_ && consumer_done_) {
      query_ = 0;
      source_ = {};
      state_ = State::kStale;
      head_ = count_ = 0;
    }
  }
};
}  // namespace football::originals::detail
