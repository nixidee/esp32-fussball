#include "wifi_policy.h"

#include <algorithm>
namespace network {
const char* stateName(State s) {
  switch (s) {
    case State::kIdle: return "idle";
    case State::kScanning: return "scanning";
    case State::kConnecting: return "connecting";
    case State::kConnected: return "connected";
    case State::kBackoffAuth: return "auth retry";
    case State::kWaitSsid: return "waiting for network";
    case State::kBackoff: return "retry";
    case State::kApOnly: return "setup AP";
  }
  return "unknown";
}
void WifiPolicy::start(bool credentials, int64_t now) {
  state_ = credentials ? State::kIdle : State::kApOnly;
  ap_ = !credentials;
  retries_ = 0;
  due_ = since_ = failed_since_ = now;
}
void WifiPolicy::scanDone(bool success, bool found, int64_t now) {
  if (state_ != State::kScanning) return;
  state_ = success && found ? State::kConnecting : State::kWaitSsid;
  since_ = now;
  due_ = success && found ? now : now + cfg::kWifiMissingScanMs;
}
void WifiPolicy::gotIp(int64_t now) {
  state_ = State::kConnected;
  ap_ = false;
  retries_ = 0;
  failed_since_ = now;
}
void WifiPolicy::lost(bool auth, bool missing, int64_t now) {
  if (state_ == State::kConnected) {
    failed_since_ = now;
    retries_ = 0;
  }
  since_ = now;
  if (auth) {
    state_ = State::kBackoffAuth;
    due_ = now + (retries_ < 3 ? cfg::kWifiAuthRetryMs[retries_]
                               : cfg::kWifiFallbackMs);
    retries_ = std::min<uint8_t>(retries_ + 1, 4);
    if (retries_ > 3) ap_ = true;
  } else if (missing) {
    state_ = State::kWaitSsid;
    due_ = now + cfg::kWifiMissingScanMs;
  } else {
    state_ = State::kBackoff;
    due_ = now + (retries_ == 0
                      ? 0
                      : std::min<uint32_t>(
                            60000, 1000u << std::min<uint8_t>(retries_, 6)));
    retries_ = std::min<uint8_t>(retries_ + 1, 7);
  }
}
Command WifiPolicy::poll(int64_t now, bool client, uint32_t jitter) {
  if (state_ == State::kConnected) {
    return Command::kNone;
  }
  if (state_ == State::kApOnly) return Command::kNone;
  if (now - failed_since_ >= cfg::kWifiFallbackMs) ap_ = true;
  if (state_ == State::kScanning && now - since_ >= cfg::kWifiScanDeadlineMs) {
    scanDone(false, false, now);
    return Command::kDisconnect;
  }
  if (state_ == State::kConnecting && now - since_ >= cfg::kWifiIpDeadlineMs) {
    lost(false, false, now);
    return Command::kDisconnect;
  }
  if (client && ap_) {
    due_ = now + cfg::kWifiMissingScanMs;
    return Command::kNone;
  }
  if (now < due_) return Command::kNone;
  if (state_ == State::kConnecting) {
    due_ = now + cfg::kWifiIpDeadlineMs;
    return Command::kConnect;
  }
  if (state_ == State::kIdle || state_ == State::kWaitSsid ||
      state_ == State::kBackoffAuth || state_ == State::kBackoff) {
    if (state_ == State::kBackoff && jitter % 1000 > 0) {
      due_ += jitter % 1000;
      state_ = State::kIdle;
      return Command::kNone;
    }
    state_ = State::kScanning;
    since_ = now;
    return Command::kScan;
  }
  return Command::kNone;
}
}  // namespace network
