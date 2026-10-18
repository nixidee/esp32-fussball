// Actual DNS loop and HTTP advertisement owner, with bounded SDK boundaries.
#include <arpa/inet.h>
#include <sys/socket.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app_config.h"
#include "runtime_readiness_sdk.h"

namespace probe {
struct Finished {};
bool malformed = false;
unsigned received = 0, sent = 0, yields = 0;
void require(bool value, const char* why) {
  if (!value) {
    std::fprintf(stderr, "FAIL %s\n", why);
    std::exit(1);
  }
}
}  // namespace probe
namespace network {
std::atomic<bool> http_ready{false};
constexpr uint32_t kHttpService = 1;
uint32_t resources = 0;
bool owns(uint32_t mask) { return (resources & mask) == mask; }
int added = 0, removed = 0;
bool fail_add = false, fail_remove = false;
esp_err_t mdns_service_add(const char*, const char*, const char*, int,
                           const void*, int) {
  ++added;
  if (fail_add) {
    fail_add = false;
    return ESP_FAIL;
  }
  return ESP_OK;
}
esp_err_t mdns_service_remove(const char*, const char*) {
  ++removed;
  if (fail_remove) {
    fail_remove = false;
    return ESP_FAIL;
  }
  return ESP_OK;
}
#include "runtime_mdns.inc"

int dns_fd = 1;
uint8_t dns_packet[512];
struct Status {
  bool ap;
};
Status status() { return {true}; }
void vTaskDelay(unsigned ticks) {
  probe::require(ticks > 0, "DNS gives scheduler a real tick");
  ++probe::yields;
}
ssize_t receive(int, void* raw, std::size_t, int, sockaddr*, socklen_t*) {
  if (probe::received == 5000) throw probe::Finished{};
  ++probe::received;
  auto* p = static_cast<uint8_t*>(raw);
  std::memset(p, 0, 19);
  if (probe::malformed) return 16;
  p[5] = 1;
  p[12] = 1;
  p[13] = 'a';
  p[16] = 1;
  p[18] = 1;
  return 19;
}
ssize_t send(int, const void* raw, std::size_t length, int, const sockaddr*,
             socklen_t) {
  const auto* p = static_cast<const uint8_t*>(raw);
  probe::require(length == 35 && p[2] == 0x81 && p[7] == 1,
                 "valid A packet retains DNS answer");
  ++probe::sent;
  return length;
}
#define recvfrom receive
#define sendto send
#include "runtime_dns.inc"
#undef recvfrom
#undef sendto
}  // namespace network
int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "mdns") == 0) {
    using namespace network;
    probe::require(applyHttpService() == ESP_OK && added == 0,
                   "no HTTP advertisement before actual readiness");
    http_ready = true;
    fail_add = true;
    probe::require(applyHttpService() == ESP_FAIL && !owns(kHttpService),
                   "failed add does not claim ownership");
    probe::require(
        applyHttpService() == ESP_OK && owns(kHttpService) && added == 2,
        "failed add retries to one owned advertisement");
    probe::require(applyHttpService() == ESP_OK && added == 2,
                   "ready advertisement is not duplicated");
    http_ready = false;
    fail_remove = true;
    probe::require(applyHttpService() == ESP_FAIL && owns(kHttpService),
                   "failed removal retains ownership for retry");
    probe::require(
        applyHttpService() == ESP_OK && !owns(kHttpService) && removed == 2,
        "unready service eventually withdraws advertisement");
    probe::require(applyHttpService() == ESP_OK && removed == 2,
                   "withdrawal is idempotent");
    std::puts(
        "PASS actual mDNS advertisement readiness/add/remove/failure recovery");
  } else {
    probe::malformed = argc > 1 && std::strcmp(argv[1], "dns_invalid") == 0;
    try {
      network::dns(nullptr);
    } catch (const probe::Finished&) {
    }
    probe::require(probe::yields >= probe::received,
                   "packet flood cannot bypass scheduler fairness");
    probe::require(probe::sent == (probe::malformed ? 0 : 5000),
                   "valid/rejected DNS replies preserved");
    std::printf("PASS DNS flood malformed=%d packets=%u yields=%u\n",
                probe::malformed, probe::received, probe::yields);
  }
}
