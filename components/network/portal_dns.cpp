#include "portal_dns.h"

#include <atomic>
#include <cerrno>
#include <cstring>

#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "network_service.h"

namespace network {
namespace {
int descriptor = -1;
uint8_t packet[cfg::kDnsPacketBytes];
StaticTask_t task_buffer;
StackType_t task_stack[cfg::kDnsStackBytes / sizeof(StackType_t)];
std::atomic<esp_err_t> runtime_error{ESP_OK};
void release() {
  const int owned = descriptor;
  descriptor = -1;
  if (owned >= 0) ::close(owned);
}
bool fail() {
  const int error = errno ? errno : EIO;
  release();
  errno = error;
  return false;
}
bool openSocket() {
  if (descriptor >= 0) return true;
  descriptor = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (descriptor < 0) return false;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(53);
  const timeval timeout{cfg::kDnsSocketWaitMs / 1000,
                        (cfg::kDnsSocketWaitMs % 1000) * 1000};
  if (::bind(descriptor, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0 ||
      ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) != 0 ||
      ::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
    fail();
    return false;
  }
  return true;
}
bool step(bool ap_enabled) {
  if (!openSocket()) return false;
  sockaddr_in source{};
  socklen_t length = sizeof(source);
  int n = ::recvfrom(descriptor, packet, sizeof(packet) - 16, 0,
                     reinterpret_cast<sockaddr*>(&source), &length);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
    return fail();
  }
  // Standard query (QR0/opcode0), one question. Preserve its original name.
  if (n < 17 || !ap_enabled || (packet[2] & 0xF8) != 0 || packet[4] != 0 ||
      packet[5] != 1)
    return true;
  int end = 12;
  while (end < n && packet[end] != 0) {
    const int size = packet[end];
    if (size > 63 || end + size + 1 >= n) {
      end = n;
      break;
    }
    end += size + 1;
  }
  if (end + 5 > n || packet[end + 3] != 0 || packet[end + 4] != 1) return true;
  // Class IN: A receives the AP address, other types an empty answer. Trailing
  // records such as EDNS are not copied into the answer.
  const bool a = packet[end + 1] == 0 && packet[end + 2] == 1;
  n = end + 5;
  packet[2] = 0x81;
  packet[3] = 0x80;
  packet[6] = 0;
  packet[7] = a ? 1 : 0;
  packet[8] = packet[9] = packet[10] = packet[11] = 0;
  const uint8_t answer[] = {0xc0,
                            0x0c,
                            0,
                            1,
                            0,
                            1,
                            0,
                            0,
                            0,
                            30,
                            0,
                            4,
                            cfg::kApIp[0],
                            cfg::kApIp[1],
                            cfg::kApIp[2],
                            cfg::kApIp[3]};
  if (a) {
    std::memcpy(packet + n, answer, sizeof(answer));
    n += sizeof(answer);
  }
  const auto sent = ::sendto(descriptor, packet, n, 0,
                             reinterpret_cast<sockaddr*>(&source), length);
  if (sent == n) return true;
  if (sent < 0 && errno == EINTR) return true;
  if (sent >= 0) errno = EIO;  // A datagram must be accepted in full.
  return fail();
}
void task(void*) {
  for (;;) {
    // Even rejected/flooded packets yield to the application loop.
    vTaskDelay(cfg::kDnsYieldTicks);
    const bool healthy = step(status().ap);
    runtime_error.store(healthy ? ESP_OK : ESP_FAIL);
    if (!healthy) {
      ESP_LOGW("network", "portal DNS socket error %d; retry in %lu s", errno,
               static_cast<unsigned long>(cfg::kDnsRetryMs / 1000));
      vTaskDelay(pdMS_TO_TICKS(cfg::kDnsRetryMs));
    }
  }
}
}  // namespace
esp_err_t startPortalDns() {
  if (!openSocket()) return ESP_FAIL;
  // On allocation failure the configured socket remains owned for retry.
  return xTaskCreateStatic(task, "portal_dns", cfg::kDnsStackBytes, nullptr,
                           cfg::kDnsTaskPriority, task_stack, &task_buffer)
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}
esp_err_t portalDnsError() { return runtime_error.load(); }
}  // namespace network
