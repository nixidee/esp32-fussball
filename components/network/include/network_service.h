#pragma once
#include <cstdint>

#include "esp_err.h"
#include "wifi_policy.h"
namespace network {
struct Status {
  State state;
  bool connected, ap;
  uint8_t ap_clients;
  int8_t rssi;
  char ip[16], ap_ip[16], ap_ssid[33];
  uint32_t epoch;
};
struct AccessPoint {
  char ssid[33];
  int8_t rssi;
  bool secure;
};
esp_err_t init();
void poll();
void applySettings();
Status status();
esp_err_t requestScan();
std::size_t scanResults(AccessPoint* out, std::size_t capacity);
bool ready();
bool healthy();
// Heavy operations use one nonblocking admission token; no waiting queue.
bool tryHeavy();
void releaseHeavy();
class HeavyGuard {
 public:
  HeavyGuard() : held_(tryHeavy()) {}
  ~HeavyGuard() {
    if (held_) releaseHeavy();
  }
  explicit operator bool() const { return held_; }

 private:
  bool held_;
};
}  // namespace network
