#pragma once
#include <cstdint>

#include "esp_err.h"
#include "wifi_policy.h"
namespace network {
// Stable local-service failure groups, shared by the API and display.
enum class InitStep : uint8_t {
  kNone = 0,
  kAntenna = 1,
  kNetworkStack = 2,
  kStationInterface = 3,
  kAccessPointInterface = 4,
  kAccessPointAddress = 5,
  kWifiDriver = 6,
  kWifiConfiguration = 7,
  kWifiEvents = 8,
  kRadio = 9,
  kPortalDns = 10,
  kMdns = 11,
  kHostname = 12,
  kHttpService = 13
};
constexpr const char* initStepKey(InitStep step) {
  switch (step) {
    case InitStep::kNone: return "";
    case InitStep::kAntenna: return "antenna";
    case InitStep::kNetworkStack: return "network_stack";
    case InitStep::kStationInterface: return "station_interface";
    case InitStep::kAccessPointInterface: return "ap_interface";
    case InitStep::kAccessPointAddress: return "ap_address";
    case InitStep::kWifiDriver: return "wifi_driver";
    case InitStep::kWifiConfiguration: return "wifi_configuration";
    case InitStep::kWifiEvents: return "wifi_events";
    case InitStep::kRadio: return "radio";
    case InitStep::kPortalDns: return "portal_dns";
    case InitStep::kMdns: return "mdns";
    case InitStep::kHostname: return "hostname";
    case InitStep::kHttpService: return "http_service";
  }
  return "";
}
constexpr const char* initStepCode(InitStep step) {
  switch (step) {
    case InitStep::kNone: return "";
    case InitStep::kAntenna: return "NET E01";
    case InitStep::kNetworkStack: return "NET E02";
    case InitStep::kStationInterface: return "NET E03";
    case InitStep::kAccessPointInterface: return "NET E04";
    case InitStep::kAccessPointAddress: return "NET E05";
    case InitStep::kWifiDriver: return "NET E06";
    case InitStep::kWifiConfiguration: return "NET E07";
    case InitStep::kWifiEvents: return "NET E08";
    case InitStep::kRadio: return "NET E09";
    case InitStep::kPortalDns: return "NET E10";
    case InitStep::kMdns: return "NET E11";
    case InitStep::kHostname: return "NET E12";
    case InitStep::kHttpService: return "NET E13";
  }
  return "";
}
struct Status {
  State state;
  bool connected, ap, initialized;
  uint8_t ap_clients;
  int8_t rssi;
  InitStep failure_step;
  char ip[16], ap_ip[16], ap_ssid[33];
  uint32_t epoch;
  esp_err_t init_error;
  // Absolute monotonic deadline; zero when no failed phase awaits retry.
  int64_t retry_at_ms;
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
struct ScanStatus {
  bool busy;               // queued or running user scan
  const char* error_code;  // empty after success; stable REST failure key
};
// Results and user-job state are copied together; no partial completion.
std::size_t scanResults(AccessPoint* out, std::size_t capacity,
                        ScanStatus* status = nullptr);
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
