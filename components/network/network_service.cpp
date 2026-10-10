#include "network_service.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_target.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "settings_store.h"
#include "time_service.h"

namespace network {
namespace {
constexpr const char* kTag = "network";
WifiPolicy policy;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Status current{};
settings::Model applied;
esp_netif_t *sta = nullptr, *ap = nullptr;
std::atomic<bool> heavy{false}, scan_done{false}, got_ip{false}, lost_ip{false};
std::atomic<uint16_t> reason{0};
std::atomic<uint8_t> clients{0};
std::atomic<bool> scan_requested{false};
bool started = false, ap_on = false, scanning = false, user_scan = false;
bool sntp_on = false, configured_radio = false, mdns_ready = false;
int64_t published_at = 0;
AccessPoint results[cfg::kScanEntries]{};
std::size_t result_count = 0;
char ntp_server[cfg::kNtpServerMaxChars + 1]{};
StaticTask_t dns_tcb;
StackType_t dns_stack[cfg::kDnsStackBytes / sizeof(StackType_t)];
int dns_fd = -1;

void publish() {
  Status next{};
  next.state = policy.state();
  next.connected = next.state == State::kConnected;
  next.ap = ap_on;
  next.ap_clients = clients.load();
  esp_netif_ip_info_t info{};
  esp_netif_get_ip_info(sta, &info);
  snprintf(next.ip, sizeof(next.ip), IPSTR, IP2STR(&info.ip));
  esp_netif_get_ip_info(ap, &info);
  snprintf(next.ap_ip, sizeof(next.ap_ip), IPSTR, IP2STR(&info.ip));
  wifi_config_t conf{};
  esp_wifi_get_config(WIFI_IF_AP, &conf);
  memcpy(next.ap_ssid, conf.ap.ssid,
         std::min<std::size_t>(conf.ap.ssid_len, 32));
  wifi_ap_record_t record{};
  if (next.connected && esp_wifi_sta_get_ap_info(&record) == ESP_OK)
    next.rssi = record.rssi;
  portENTER_CRITICAL(&mux);
  const bool changed =
      next.connected != current.connected || next.ap != current.ap;
  next.epoch = current.epoch + (changed ? 1 : 0);
  current = next;
  portEXIT_CRITICAL(&mux);
  if (changed) events::post(events::Event::kNetworkState);
}
void event(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_SCAN_DONE) scan_done.store(true);
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
      reason.store(static_cast<wifi_event_sta_disconnected_t*>(data)->reason);
      lost_ip.store(true);
    }
    if (id == WIFI_EVENT_AP_STACONNECTED) clients.fetch_add(1);
    if (id == WIFI_EVENT_AP_STADISCONNECTED && clients.load() > 0)
      clients.fetch_sub(1);
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    got_ip.store(true);
}
void dns(void*) {
  const int fd = dns_fd;
  timeval timeout{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  uint8_t packet[512];
  for (;;) {
    sockaddr_in source{};
    socklen_t length = sizeof(source);
    int n = recvfrom(fd, packet, sizeof(packet) - 16, 0,
                     reinterpret_cast<sockaddr*>(&source), &length);
    if (n < 17 || !status().ap || packet[4] != 0 || packet[5] != 1) continue;
    int end = 12;
    while (end < n && packet[end] != 0) {
      const int size = packet[end];
      if (size > 63 || end + size + 1 >= n) {
        end = n;
        break;
      }
      end += size + 1;
    }
    if (end + 5 != n || packet[end + 1] != 0 || packet[end + 2] != 1 ||
        packet[end + 3] != 0 || packet[end + 4] != 1)
      continue;
    packet[2] = 0x81;
    packet[3] = 0x80;
    packet[6] = 0;
    packet[7] = 1;
    packet[8] = packet[9] = packet[10] = packet[11] = 0;
    uint8_t answer[] = {0xc0,
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
    memcpy(packet + n, answer, sizeof(answer));
    sendto(fd, packet, n + sizeof(answer), 0,
           reinterpret_cast<sockaddr*>(&source), length);
  }
}
esp_err_t antenna(bool external) {
  if (!hw::kBoard.has_antenna_switch) return ESP_OK;
  const auto& b = hw::kBoard;
  gpio_config_t config{};
  config.pin_bit_mask =
      (1ULL << b.antenna_enable_pin) | (1ULL << b.antenna_select_pin);
  config.mode = GPIO_MODE_OUTPUT;
  esp_err_t err = gpio_config(&config);
  if (err == ESP_OK)
    err = gpio_set_level(static_cast<gpio_num_t>(b.antenna_enable_pin),
                         b.antenna_enable_active_high);
  vTaskDelay(pdMS_TO_TICKS(cfg::kAntennaSettleMs));
  if (err == ESP_OK)
    err = gpio_set_level(static_cast<gpio_num_t>(b.antenna_select_pin),
                         external == b.antenna_external_level_high);
  return err;
}
void sntp() {
  if (sntp_on) return;
  memcpy(ntp_server, applied.ntp_server.data.data(), applied.ntp_server.length);
  ntp_server[applied.ntp_server.length] = 0;
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp_server);
  const esp_err_t err = esp_netif_sntp_init(&config);
  sntp_on = err == ESP_OK;
  if (err != ESP_OK) ESP_LOGE(kTag, "SNTP init: %s", esp_err_to_name(err));
}
}  // namespace
esp_err_t init() {
  applied = settings::current();
  esp_err_t err = antenna(applied.external_antenna);
  if (err != ESP_OK) return err;
  if ((err = esp_netif_init()) != ESP_OK) return err;
  if ((err = esp_event_loop_create_default()) != ESP_OK &&
      err != ESP_ERR_INVALID_STATE)
    return err;
  sta = esp_netif_create_default_wifi_sta();
  ap = esp_netif_create_default_wifi_ap();
  if (sta == nullptr || ap == nullptr) return ESP_ERR_NO_MEM;
  esp_netif_ip_info_t ap_address{};
  IP4_ADDR(&ap_address.ip, cfg::kApIp[0], cfg::kApIp[1], cfg::kApIp[2],
           cfg::kApIp[3]);
  ap_address.gw = ap_address.ip;
  IP4_ADDR(&ap_address.netmask, 255, 255, 255, 0);
  // A new AP is DHCP_INIT, but setting its address requires DHCP_STOPPED.
  // Re-arm DHCP before radio startup so the AP's netif-up event starts it.
  if ((err = esp_netif_dhcps_stop(ap)) != ESP_OK) return err;
  if ((err = esp_netif_set_ip_info(ap, &ap_address)) != ESP_OK) return err;
  if ((err = esp_netif_dhcps_start(ap)) != ESP_OK) return err;
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  if ((err = esp_wifi_init(&config)) != ESP_OK) return err;
  if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
  ESP_ERROR_CHECK(
      esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             event, nullptr));
  started = true;
  applySettings();
  dns_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in dns_address{};
  dns_address.sin_family = AF_INET;
  dns_address.sin_port = htons(53);
  if (dns_fd < 0 || bind(dns_fd, reinterpret_cast<sockaddr*>(&dns_address),
                         sizeof(dns_address)) != 0) {
    if (dns_fd >= 0) close(dns_fd);
    dns_fd = -1;
    ESP_LOGE(kTag, "portal DNS socket/bind failed");
    return ESP_FAIL;
  }
  if (xTaskCreateStatic(dns, "portal_dns", cfg::kDnsStackBytes, nullptr, 2,
                        dns_stack, &dns_tcb) == nullptr) {
    close(dns_fd);
    dns_fd = -1;
    return ESP_ERR_NO_MEM;
  }
  if ((err = mdns_init()) != ESP_OK) return err;
  mdns_ready = true;
  char hostname[64]{};
  memcpy(hostname, applied.hostname.data.data(), applied.hostname.length);
  if ((err = mdns_hostname_set(hostname)) != ESP_OK) return err;
  return mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
}
bool healthy() { return started && mdns_ready && dns_fd >= 0; }
void applySettings() {
  if (!started) return;
  auto model = settings::current();
  const bool radio_changed = !configured_radio ||
                             model.wifi_ssid != applied.wifi_ssid ||
                             model.wifi_password != applied.wifi_password ||
                             model.ap_password != applied.ap_password ||
                             model.external_antenna != applied.external_antenna;
  if (!radio_changed) {
    const bool hostname_changed = model.hostname != applied.hostname;
    const bool ntp_changed = model.ntp_server != applied.ntp_server;
    applied = model;
    if (hostname_changed) {
      char hostname[64]{};
      memcpy(hostname, model.hostname.data.data(), model.hostname.length);
      esp_netif_set_hostname(sta, hostname);
      if (mdns_ready) mdns_hostname_set(hostname);
    }
    if (ntp_changed) {
      if (sntp_on) esp_netif_sntp_deinit();
      sntp_on = false;
      if (ready()) sntp();
    }
    return;
  }
  // Reconfigure the radio while stopped; antenna changes always reconnect.
  esp_wifi_scan_stop();
  esp_wifi_stop();
  scanning = false;
  user_scan = false;
  if (sntp_on) {
    esp_netif_sntp_deinit();
    sntp_on = false;
  }
  ESP_ERROR_CHECK(antenna(model.external_antenna));
  applied = model;
  wifi_config_t station{};
  memcpy(station.sta.ssid, model.wifi_ssid.data.data(), model.wifi_ssid.length);
  memcpy(station.sta.password, model.wifi_password.data.data(),
         model.wifi_password.length);
  station.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  station.sta.threshold.authmode = WIFI_AUTH_OPEN;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
  wifi_config_t access{};
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(reinterpret_cast<char*>(access.ap.ssid), sizeof(access.ap.ssid),
           "%s-%02X%02X%02X", cfg::kApNamePrefix, mac[3], mac[4], mac[5]);
  access.ap.ssid_len = strlen(reinterpret_cast<char*>(access.ap.ssid));
  memcpy(access.ap.password, model.ap_password.data.data(),
         model.ap_password.length);
  access.ap.authmode =
      model.ap_password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  access.ap.max_connection = cfg::kApClients;
  access.ap.channel = cfg::kApChannel;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &access));
  char hostname[64]{};
  memcpy(hostname, model.hostname.data.data(), model.hostname.length);
  esp_netif_set_hostname(sta, hostname);
  if (mdns_ready) mdns_hostname_set(hostname);
  policy.start(!model.wifi_ssid.empty(), timekeeping::monotonicMs());
  ap_on = policy.ap();
  clients.store(0);
  lost_ip.store(false);
  got_ip.store(false);
  scan_done.store(false);
  ESP_ERROR_CHECK(esp_wifi_set_mode(ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  configured_radio = true;
  publish();
}
void poll() {
  if (!started) return;
  const int64_t now = timekeeping::monotonicMs();
  if (scan_requested.exchange(false) && !scanning &&
      policy.state() != State::kConnecting &&
      policy.state() != State::kScanning) {
    wifi_scan_config_t scan{};
    scan.show_hidden = true;
    if (esp_wifi_scan_start(&scan, false) == ESP_OK) {
      scanning = true;
      user_scan = true;
    }
  }
  if (got_ip.exchange(false)) {
    policy.gotIp(now);
    sntp();
  }
  if (lost_ip.exchange(false)) {
    const auto r = reason.load();
    policy.lost(r == WIFI_REASON_AUTH_FAIL ||
                    r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                    r == WIFI_REASON_HANDSHAKE_TIMEOUT,
                r == WIFI_REASON_NO_AP_FOUND, now);
  }
  if (scan_done.exchange(false)) {
    wifi_ap_record_t records[cfg::kScanEntries]{};
    uint16_t count = cfg::kScanEntries;
    bool ok = esp_wifi_scan_get_ap_records(&count, records) == ESP_OK;
    bool found = false;
    portENTER_CRITICAL(&mux);
    result_count = ok ? count : 0;
    for (uint16_t i = 0; i < count && ok; ++i) {
      memcpy(results[i].ssid, records[i].ssid, 32);
      results[i].ssid[32] = 0;
      results[i].rssi = records[i].rssi;
      results[i].secure = records[i].authmode != WIFI_AUTH_OPEN;
      found |= !applied.wifi_ssid.empty() &&
               memcmp(records[i].ssid, applied.wifi_ssid.data.data(),
                      applied.wifi_ssid.length) == 0 &&
               (applied.wifi_ssid.length == 32 ||
                records[i].ssid[applied.wifi_ssid.length] == 0);
    }
    portEXIT_CRITICAL(&mux);
    scanning = false;
    if (!user_scan) policy.scanDone(ok, found, now);
    user_scan = false;
  }
  const auto command = policy.poll(now, clients.load() > 0, esp_random());
  if (policy.ap() != ap_on) {
    ap_on = policy.ap();
    const auto err = esp_wifi_set_mode(ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) ESP_LOGE(kTag, "mode: %s", esp_err_to_name(err));
  }
  esp_err_t err = ESP_OK;
  if (command == Command::kScan) {
    wifi_scan_config_t scan{};
    scan.show_hidden = true;
    // The connection scan targets the configured SSID so a weaker AP cannot
    // disappear behind the bounded list of stronger unrelated networks.
    uint8_t ssid[cfg::kWifiSsidMaxBytes + 1]{};
    memcpy(ssid, applied.wifi_ssid.data.data(), applied.wifi_ssid.length);
    scan.ssid = ssid;
    err = esp_wifi_scan_start(&scan, false);
    scanning = err == ESP_OK;
    if (err != ESP_OK) policy.scanDone(false, false, now);
  } else if (command == Command::kConnect) {
    err = esp_wifi_connect();
    if (err != ESP_OK) policy.lost(false, false, now);
  } else if (command == Command::kDisconnect) {
    esp_wifi_scan_stop();
    scanning = false;
    esp_wifi_disconnect();
  }
  if (err != ESP_OK) ESP_LOGW(kTag, "operation: %s", esp_err_to_name(err));
  if (now - published_at >= cfg::kNetworkPublishMs ||
      status().state != policy.state() || status().ap != ap_on ||
      status().ap_clients != clients.load()) {
    published_at = now;
    publish();
  }
}
Status status() {
  portENTER_CRITICAL(&mux);
  const auto copy = current;
  portEXIT_CRITICAL(&mux);
  return copy;
}
bool ready() { return status().connected; }
esp_err_t requestScan() {
  if (!started) return ESP_ERR_INVALID_STATE;
  scan_requested.store(true);
  return ESP_OK;
}
std::size_t scanResults(AccessPoint* out, std::size_t capacity) {
  portENTER_CRITICAL(&mux);
  const auto count = std::min(capacity, result_count);
  memcpy(out, results, count * sizeof(AccessPoint));
  portEXIT_CRITICAL(&mux);
  return count;
}
bool tryHeavy() {
  bool expected = false;
  return heavy.compare_exchange_strong(expected, true);
}
void releaseHeavy() { heavy.store(false); }
}  // namespace network

// ESP-IDF documents this weak hook for applications owning clock updates.
extern "C" void sntp_sync_time(struct timeval* tv) {
  timekeeping::setTime(*tv, timekeeping::Source::kSntp);
}
