#pragma once
#include <cstdint>

#include "app_config.h"

namespace network {
enum class State : uint8_t {
  kIdle,
  kScanning,
  kConnecting,
  kConnected,
  kBackoffAuth,
  kWaitSsid,
  kBackoff,
  kApOnly
};
enum class Command : uint8_t { kNone, kScan, kConnect, kDisconnect };
class WifiPolicy {
 public:
  void start(bool credentials, int64_t now);
  void scanDone(bool success, bool found, int64_t now);
  void gotIp(int64_t now);
  void lost(bool auth, bool missing, int64_t now);
  Command poll(int64_t now, bool ap_client, uint32_t jitter);
  State state() const { return state_; }
  bool ap() const { return ap_; }

 private:
  State state_ = State::kIdle;
  int64_t since_ = 0, due_ = 0, failed_since_ = 0;
  uint8_t retries_ = 0;
  bool ap_ = false;
};
const char* stateName(State state);
}  // namespace network
