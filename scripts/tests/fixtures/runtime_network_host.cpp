// Actual DNS loop and HTTP advertisement owner, with bounded SDK boundaries.
#include <arpa/inet.h>
#include <sys/socket.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "app_config.h"
#include "network_service.h"
#include "portal_dns.h"
#include "portal_dns_task_sdk.h"
#include "runtime_readiness_sdk.h"

namespace probe {
struct Finished {};
bool malformed = false;
unsigned received = 0, sent = 0, yields = 0;
unsigned opens = 0, closes = 0, failures = 0;
bool fail_receive = false, fail_send = false;
enum class Fault { kNone, kOpen, kBind, kReceiveWait, kSendWait, kShortSend };
Fault fault = Fault::kNone;
int receive_error = 0, send_error = 0, active_socket = -1;
unsigned pauses = 0;
bool ap_enabled = true, non_a = false, bad_label = false;
bool injected = false;
void (*task_entry)(void*) = nullptr;
unsigned tasks = 0;
bool fail_task = false;
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

int openSocket(int family, int type, int protocol) {
  probe::require(
      family == AF_INET && type == SOCK_DGRAM && protocol == IPPROTO_UDP,
      "DNS opens only its UDP socket");
  ++probe::opens;
  if (probe::fault == probe::Fault::kOpen && !probe::injected) {
    probe::injected = true;
    errno = ENOMEM;
    return -1;
  }
  probe::require(probe::active_socket < 0, "one owned socket at a time");
  probe::active_socket = probe::opens;
  return probe::opens;
}
int bindSocket(int, const sockaddr* raw, socklen_t) {
  const auto& address = *reinterpret_cast<const sockaddr_in*>(raw);
  probe::require(ntohs(address.sin_port) == 53, "portal DNS port");
  if (probe::fault == probe::Fault::kBind && !probe::injected) {
    probe::injected = true;
    errno = EADDRINUSE;
    return -1;
  }
  return 0;
}
int configureSocket(int, int level, int option, const void* raw, socklen_t) {
  const auto& timeout = *static_cast<const timeval*>(raw);
  probe::require(level == SOL_SOCKET &&
                     (option == SO_RCVTIMEO || option == SO_SNDTIMEO) &&
                     timeout.tv_sec * 1000 + timeout.tv_usec / 1000 ==
                         cfg::kDnsSocketWaitMs,
                 "DNS send and receive waits are bounded");
  if (((probe::fault == probe::Fault::kReceiveWait && option == SO_RCVTIMEO) ||
       (probe::fault == probe::Fault::kSendWait && option == SO_SNDTIMEO)) &&
      !probe::injected) {
    probe::injected = true;
    errno = EIO;
    return -1;
  }
  return 0;
}
int closeSocket(int fd) {
  probe::require(fd == probe::active_socket,
                 "close only the owned socket once");
  probe::active_socket = -1;
  ++probe::closes;
  return 0;
}
Status status() {
  Status value{};
  value.ap = probe::ap_enabled;
  return value;
}
void delay(unsigned ticks) {
  probe::require(ticks > 0, "DNS gives scheduler a real tick");
  ++probe::yields;
  if (ticks == cfg::kDnsRetryMs) {
    ++probe::pauses;
    probe::require(portalDnsError() == ESP_FAIL,
                   "runtime DNS failure published before retry pause");
  }
}
ssize_t receive(int fd, void* raw, std::size_t, int, sockaddr*, socklen_t*) {
  probe::require(fd == probe::active_socket, "receive uses the current socket");
  if (probe::fail_receive && probe::failures++ == 0) {
    errno = EBADF;
    return -1;
  }
  if (probe::receive_error && !probe::injected) {
    probe::injected = true;
    errno = probe::receive_error;
    return -1;
  }
  if (probe::received == 5000) throw probe::Finished{};
  ++probe::received;
  auto* p = static_cast<uint8_t*>(raw);
  std::memset(p, 0, 19);
  if (probe::malformed) return 16;
  p[5] = 1;
  p[12] = probe::bad_label ? 64 : 1;
  p[13] = 'a';
  p[16] = probe::non_a ? 28 : 1;
  p[18] = 1;
  return 19;
}
ssize_t send(int fd, const void* raw, std::size_t length, int, const sockaddr*,
             socklen_t) {
  probe::require(fd == probe::active_socket, "send uses the current socket");
  const auto* p = static_cast<const uint8_t*>(raw);
  probe::require(length == (probe::non_a ? 19u : 35u) && p[2] == 0x81 &&
                     p[7] == (probe::non_a ? 0 : 1),
                 "valid A/empty non-A DNS answer");
  if (!probe::non_a)
    probe::require(std::memcmp(p + 31, cfg::kApIp, 4) == 0,
                   "answer contains configured AP address");
  ++probe::sent;
  if (probe::fail_send && probe::failures++ == 0) {
    errno = EIO;
    return -1;
  }
  if (probe::send_error && !probe::injected) {
    probe::injected = true;
    errno = probe::send_error;
    return -1;
  }
  if (probe::fault == probe::Fault::kShortSend && !probe::injected) {
    probe::injected = true;
    return length - 1;
  }
  return length;
}
}  // namespace network
void vTaskDelay(uint32_t ticks) { network::delay(ticks); }
void* xTaskCreateStatic(void (*entry)(void*), const char* name, uint32_t bytes,
                        void* context, unsigned priority, StackType_t* stack,
                        StaticTask_t* buffer) {
  ++probe::tasks;
  probe::require(std::strcmp(name, "portal_dns") == 0 &&
                     bytes == cfg::kDnsStackBytes && context == nullptr &&
                     priority == cfg::kDnsTaskPriority && stack && buffer,
                 "same bounded statically allocated task");
  if (probe::fail_task) {
    probe::fail_task = false;
    return nullptr;
  }
  probe::require(!probe::task_entry, "never create a second DNS task");
  probe::task_entry = entry;
  return reinterpret_cast<void*>(1);
}
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
    probe::fail_receive = argc > 1 && std::strcmp(argv[1], "dns_receive") == 0;
    probe::fail_send = argc > 1 && std::strcmp(argv[1], "dns_send") == 0;
    const std::string_view name = argc > 1 ? argv[1] : "";
    if (name == "dns_open") probe::fault = probe::Fault::kOpen;
    if (name == "dns_bind") probe::fault = probe::Fault::kBind;
    if (name == "dns_recv_option") probe::fault = probe::Fault::kReceiveWait;
    if (name == "dns_send_option") probe::fault = probe::Fault::kSendWait;
    if (name == "dns_short_send") probe::fault = probe::Fault::kShortSend;
    if (name == "dns_timeout") probe::receive_error = EAGAIN;
    if (name == "dns_interrupt") probe::receive_error = EINTR;
    if (name == "dns_send_interrupt") probe::send_error = EINTR;
    probe::ap_enabled = name != "dns_disabled";
    probe::non_a = name == "dns_non_a";
    probe::bad_label = name == "dns_bad_label";
    probe::fail_task = name == "dns_task";
    const bool startup_fault = probe::fault != probe::Fault::kNone &&
                               probe::fault != probe::Fault::kShortSend;
    const auto start_error = probe::fail_task ? ESP_ERR_NO_MEM : ESP_FAIL;
    if (startup_fault || probe::fail_task)
      probe::require(
          network::startPortalDns() == start_error && !probe::task_entry,
          "failed socket/task start never claims a running task");
    probe::require(network::startPortalDns() == ESP_OK && probe::task_entry,
                   "existing initialization can retry the DNS owner");
    const bool runtime_open_fault = name == "dns_runtime_open";
    if (runtime_open_fault) {
      probe::fault = probe::Fault::kOpen;
      probe::fail_receive = true;
    }
    try {
      probe::task_entry(nullptr);
    } catch (const probe::Finished&) {
    }
    probe::require(probe::yields >= probe::received,
                   "packet flood cannot bypass scheduler fairness");
    const bool rejected =
        probe::malformed || probe::bad_label || !probe::ap_enabled;
    probe::require(probe::sent == (rejected ? 0 : 5000),
                   "valid/rejected DNS replies preserved");
    const bool failed = probe::fail_receive || probe::fail_send ||
                        probe::fault != probe::Fault::kNone;
    if (runtime_open_fault) {
      probe::require(
          probe::closes == 1 && probe::opens == 3 && probe::pauses == 2,
          "failed running reopen retries without a second task");
    } else if (failed) {
      const auto expected_closes = probe::fault == probe::Fault::kOpen ? 0 : 1;
      probe::require(probe::closes == expected_closes && probe::opens == 2 &&
                         probe::pauses == (startup_fault ? 0 : 1),
                     "failed running socket closed and replaced");
    } else
      probe::require(
          probe::opens == 1 && probe::closes == 0 && probe::pauses == 0,
          "valid packets/timeouts/interruptions retain owned socket");
    probe::require(probe::tasks == (name == "dns_task" ? 2u : 1u),
                   "only failed task creation is repeated");
    probe::require(network::portalDnsError() == ESP_OK,
                   "runtime DNS state recovers after socket repair");
    std::printf("PASS DNS flood malformed=%d packets=%u yields=%u\n",
                probe::malformed, probe::received, probe::yields);
  }
}
