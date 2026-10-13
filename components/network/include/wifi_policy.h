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
// Why the device is not on the home network, for people: connecting
// (idle, scanning, associating, retry after a drop), saved network not
// found, password probably wrong (authentication failed), setup AP only
// (no saved network). kNone while connected.
enum class Reason : uint8_t {
  kNone,
  kConnecting,
  kNotFound,
  kPassword,
  kSetup
};
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
  // Start of the current postponement for an AP client (held_).
  int64_t held_since_ = 0;
  uint8_t retries_ = 0;
  bool ap_ = false, held_ = false;
};
const char* stateName(State state);
Reason offlineReason(State state);
// Stable lower-case key of a reason (REST API); "" for kNone.
const char* reasonName(Reason reason);
}  // namespace network
