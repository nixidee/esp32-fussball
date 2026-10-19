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
#include "esp_netif_defaults.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_target.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"
#include "operation_admission.h"
#include "portal_dns.h"
#include "settings_store.h"
#include "time_service.h"

namespace network {
namespace {
constexpr const char* kTag = "network";
WifiPolicy policy;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Status current{};
settings::NetworkSettings applied;
esp_netif_t *sta = nullptr, *ap = nullptr;
std::atomic<bool> http_ready{false};
// Last station transition since the previous poll(): the later event wins,
// so "lost, then got an IP" ends connected and the reverse disconnected.
enum class Station : uint8_t { kNone, kGotIp, kLost };
std::atomic<Station> station_event{Station::kNone};
std::atomic<int> scan_done{-1};  // -1 = none, SDK scan status 0 = successful
std::atomic<uint16_t> reason{0};
std::atomic<uint8_t> clients{0};
enum class UserScanState : uint8_t {
  kIdle,
  kQueued,
  kRunning,
  kStartFailed,
  kResultFailed,
  kInterrupted,
  kTimeout
};
UserScanState scan_state = UserScanState::kIdle;  // protected by mux
int64_t user_scan_at = 0;
bool started = false, ap_on = false, scanning = false, user_scan = false;
bool sntp_on = false, configured_radio = false, settings_pending = true;
int64_t published_at = 0, mode_retry_at = 0;
// Confirmed substeps retain their resources for this boot; the global stack
// and possibly borrowed loop are only acknowledged, never destroyed. Retries
// never repeat successful constructors, registrations, socket binds or tasks.
enum Resource : uint32_t {
  kPins = 1u << 0,
  kStack = 1u << 1,
  kLoop = 1u << 2,
  kStation = 1u << 3,
  kAccessPoint = 1u << 4,
  kDhcpStopped = 1u << 5,
  kApAddress = 1u << 6,
  kDhcpArmed = 1u << 7,
  kDriver = 1u << 8,
  kStorage = 1u << 9,
  kPowerSave = 1u << 10,
  kWifiHandler = 1u << 11,
  kIpHandler = 1u << 12,
  kRunning = 1u << 13,
  kDnsTask = 1u << 14,
  kMdns = 1u << 15,
  kDhcpHostname = 1u << 16,
  kMdnsHostname = 1u << 17,
  kHttpService = 1u << 18
};
constexpr uint32_t kRadioPrerequisites =
    kPins | kStack | kLoop | kStation | kAccessPoint | kDhcpStopped |
    kApAddress | kDhcpArmed | kDriver | kStorage | kPowerSave | kWifiHandler |
    kIpHandler;
constexpr uint32_t kRequiredResources = kRadioPrerequisites | kRunning |
                                        kDnsTask | kMdns | kDhcpHostname |
                                        kMdnsHostname;
uint32_t resources = 0;
esp_err_t init_errors[static_cast<unsigned>(InitStep::kHttpService) + 1]{};
int64_t init_retry_at = 0;
bool owns(uint32_t mask) { return (resources & mask) == mask; }
bool initialized() {
  return owns(kRequiredResources) && configured_radio && !settings_pending &&
         http_ready.load() == owns(kHttpService) &&
         init_errors[static_cast<unsigned>(InitStep::kHttpService)] == ESP_OK &&
         init_errors[static_cast<unsigned>(InitStep::kPortalDns)] == ESP_OK;
}
esp_err_t& initError(InitStep step) {
  return init_errors[static_cast<unsigned>(step)];
}
InitStep failureStep() {
  for (unsigned i = 1; i < std::size(init_errors); ++i)
    if (init_errors[i] != ESP_OK) return static_cast<InitStep>(i);
  return InitStep::kNone;
}
// esp_netif_sntp_init() failed (it allocates); retried while connected.
bool sntp_pending = false;
int64_t sntp_retry_at = 0;
AccessPoint results[cfg::kScanEntries]{};
std::size_t result_count = 0;
char ntp_server[cfg::kNtpServerMaxChars + 1]{};

bool userScanBusy(UserScanState state) {
  return state == UserScanState::kQueued || state == UserScanState::kRunning;
}
bool scanQueued() {
  portENTER_CRITICAL(&mux);
  const bool queued = scan_state == UserScanState::kQueued;
  portEXIT_CRITICAL(&mux);
  return queued;
}
void setScanState(UserScanState state) {
  portENTER_CRITICAL(&mux);
  scan_state = state;
  portEXIT_CRITICAL(&mux);
}
void cancelUserScan(UserScanState state = UserScanState::kInterrupted) {
  portENTER_CRITICAL(&mux);
  if (userScanBusy(scan_state)) scan_state = state;
  portEXIT_CRITICAL(&mux);
  if (!user_scan) return;
  esp_wifi_scan_stop();
  scanning = false;
  user_scan = false;
  scan_done.store(-1);
}

void publish() {
  Status next{};
  next.state = owns(kRunning) ? policy.state() : State::kIdle;
  next.connected = next.state == State::kConnected;
  next.ap = ap_on;
  next.ap_clients = clients.load();
  next.initialized = initialized();
  next.failure_step = failureStep();
  next.init_error = initError(next.failure_step);
  next.retry_at_ms = next.failure_step == InitStep::kNone ? 0 : init_retry_at;
  esp_netif_ip_info_t info{};
  if (owns(kStation)) esp_netif_get_ip_info(sta, &info);
  snprintf(next.ip, sizeof(next.ip), IPSTR, IP2STR(&info.ip));
  info = {};
  if (owns(kAccessPoint)) esp_netif_get_ip_info(ap, &info);
  snprintf(next.ap_ip, sizeof(next.ap_ip), IPSTR, IP2STR(&info.ip));
  wifi_config_t conf{};
  if (owns(kDriver)) esp_wifi_get_config(WIFI_IF_AP, &conf);
  memcpy(next.ap_ssid, conf.ap.ssid,
         std::min<std::size_t>(conf.ap.ssid_len, 32));
  wifi_ap_record_t record{};
  if (next.connected && esp_wifi_sta_get_ap_info(&record) == ESP_OK)
    next.rssi = record.rssi;
  portENTER_CRITICAL(&mux);
  const bool connectivity_changed =
      next.connected != current.connected || next.ap != current.ap;
  const bool changed = connectivity_changed ||
                       next.initialized != current.initialized ||
                       next.failure_step != current.failure_step ||
                       next.init_error != current.init_error;
  next.epoch = current.epoch + (connectivity_changed ? 1 : 0);
  current = next;
  portEXIT_CRITICAL(&mux);
  if (changed) events::post(events::Event::kNetworkState);
}
void event(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_SCAN_DONE)
      scan_done.store(static_cast<wifi_event_sta_scan_done_t*>(data)->status);
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
      reason.store(static_cast<wifi_event_sta_disconnected_t*>(data)->reason);
      station_event.store(Station::kLost);
    }
    if (id == WIFI_EVENT_AP_STACONNECTED) clients.fetch_add(1);
    if (id == WIFI_EVENT_AP_STADISCONNECTED && clients.load() > 0)
      clients.fetch_sub(1);
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    station_event.store(Station::kGotIp);
}
// Once at init: gpio_config reserves output pins, and configuring a reserved
// pin again only logs "conflict found". antenna() then just sets levels.
esp_err_t antennaPins() {
  if (!hw::kBoard.has_antenna_switch) return ESP_OK;
  const auto& b = hw::kBoard;
  gpio_config_t config{};
  config.pin_bit_mask =
      (1ULL << b.antenna_enable_pin) | (1ULL << b.antenna_select_pin);
  config.mode = GPIO_MODE_OUTPUT;
  return gpio_config(&config);
}
esp_err_t antenna(bool external) {
  if (!hw::kBoard.has_antenna_switch) return ESP_OK;
  const auto& b = hw::kBoard;
  esp_err_t err = gpio_set_level(static_cast<gpio_num_t>(b.antenna_enable_pin),
                                 b.antenna_enable_active_high);
  vTaskDelay(pdMS_TO_TICKS(cfg::kAntennaSettleMs));
  if (err == ESP_OK)
    err = gpio_set_level(static_cast<gpio_num_t>(b.antenna_select_pin),
                         external == b.antenna_external_level_high);
  return err;
}
// Initial setup and later changes use this single path. Each allocating setter
// is acknowledged separately, so retrying mDNS does not replace a good DHCP
// name.
esp_err_t applyHostname() {
  char hostname[decltype(settings::NetworkSettings::hostname)::kCapacity + 1]{};
  memcpy(hostname, applied.hostname.data.data(), applied.hostname.length);
  esp_err_t first = ESP_OK;
  if (owns(kStation) && !owns(kDhcpHostname)) {
    first = esp_netif_set_hostname(sta, hostname);
    if (first == ESP_OK) resources |= kDhcpHostname;
  }
  if (owns(kMdns) && !owns(kMdnsHostname)) {
    const auto err = mdns_hostname_set(hostname);
    if (err == ESP_OK) resources |= kMdnsHostname;
    if (first == ESP_OK) first = err;
  }
  return first;
}
// Called by the existing initialization owner after mDNS/hostname setup.
// The owned bit follows the SDK result, so failed removal is retried too.
esp_err_t applyHttpService() {
  const bool want = http_ready.load();
  if (want == owns(kHttpService)) return ESP_OK;
  const auto err =
      want ? mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0)
           : mdns_service_remove("_http", "_tcp");
  if (err == ESP_OK) {
    if (want)
      resources |= kHttpService;
    else
      resources &= ~kHttpService;
  }
  return err;
}
void sntp() {
  if (sntp_on) return;
  memcpy(ntp_server, applied.ntp_server.data.data(), applied.ntp_server.length);
  ntp_server[applied.ntp_server.length] = 0;
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp_server);
  const esp_err_t err = esp_netif_sntp_init(&config);
  sntp_on = err == ESP_OK;
  sntp_pending = !sntp_on;
  if (sntp_on) return;
  sntp_retry_at = timekeeping::monotonicMs() + cfg::kWifiRadioRetryMs;
  ESP_LOGE(kTag, "SNTP init: %s; retry in %lu s", esp_err_to_name(err),
           static_cast<unsigned long>(cfg::kWifiRadioRetryMs / 1000));
}
// Driver steps of a radio reconfiguration; 'step' names the failed one.
esp_err_t configureRadio(const settings::NetworkSettings& model,
                         const char*& step) {
  esp_err_t err;
  step = "antenna";
  if ((err = antenna(model.external_antenna)) != ESP_OK) return err;
  wifi_config_t station{};
  memcpy(station.sta.ssid, model.wifi_ssid.data.data(), model.wifi_ssid.length);
  memcpy(station.sta.password, model.wifi_password.data.data(),
         model.wifi_password.length);
  station.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  station.sta.threshold.authmode = WIFI_AUTH_OPEN;
  step = "mode";
  if ((err = esp_wifi_set_mode(WIFI_MODE_APSTA)) != ESP_OK) return err;
  step = "station config";
  if ((err = esp_wifi_set_config(WIFI_IF_STA, &station)) != ESP_OK) return err;
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
  step = "AP config";
  if ((err = esp_wifi_set_config(WIFI_IF_AP, &access)) != ESP_OK) return err;
  policy.start(!model.wifi_ssid.empty(), timekeeping::monotonicMs());
  // The radio is stopped: no AP until the driver started in the new mode.
  const bool want_ap = policy.ap();
  station_event.store(Station::kNone);
  scan_done.store(-1);
  step = "mode";
  if ((err = esp_wifi_set_mode(want_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA)) !=
      ESP_OK)
    return err;
  step = "start";
  if ((err = esp_wifi_start()) != ESP_OK) return err;
  resources |= kRunning;
  ap_on = want_ap;
  return ESP_OK;
}
// A failed attach/handler setup retains ownership until cleanup succeeds.
// The default convenience constructors abort on these errors in IDF 6.1.
esp_err_t createInterface(esp_netif_t*& netif, bool station) {
  const auto clear = [&]() {
    const auto err = esp_wifi_clear_default_wifi_driver_and_handlers(netif);
    if (err == ESP_OK) {
      esp_netif_destroy(netif);
      netif = nullptr;
    }
    return err;
  };
  if (netif != nullptr) {
    const auto err = clear();
    if (err != ESP_OK) return err;
  }
  esp_netif_config_t config = ESP_NETIF_DEFAULT_WIFI_STA();
  if (!station) config = ESP_NETIF_DEFAULT_WIFI_AP();
  netif = esp_netif_new(&config);
  if (netif == nullptr) return ESP_ERR_NO_MEM;
  esp_err_t err = station ? esp_netif_attach_wifi_station(netif)
                          : esp_netif_attach_wifi_ap(netif);
  if (err == ESP_OK)
    err = station ? esp_wifi_set_default_wifi_sta_handlers()
                  : esp_wifi_set_default_wifi_ap_handlers();
  if (err != ESP_OK) {
    const auto cleanup = clear();
    if (cleanup != ESP_OK) return cleanup;
  }
  return err;
}

void adoptModel(const settings::NetworkSettings& model) {
  if (model.hostname != applied.hostname) {
    resources &= ~(kDhcpHostname | kMdnsHostname);
    initError(InitStep::kHostname) = ESP_OK;
  }
  const bool ntp_changed = model.ntp_server != applied.ntp_server;
  applied = model;
  if (ntp_changed) {
    if (sntp_on) esp_netif_sntp_deinit();
    sntp_on = false;
    sntp_pending = false;
    if (ready()) sntp();
  }
}

esp_err_t applyModel(const settings::NetworkSettings& model) {
  const bool radio_changed = !configured_radio ||
                             model.wifi_ssid != applied.wifi_ssid ||
                             model.wifi_password != applied.wifi_password ||
                             model.ap_password != applied.ap_password ||
                             model.external_antenna != applied.external_antenna;
  if (!radio_changed) {
    adoptModel(model);
    return ESP_OK;
  }
  cancelUserScan();
  esp_wifi_scan_stop();
  scanning = false;
  user_scan = false;
  const char* step = "stop";
  esp_err_t err = esp_wifi_stop();
  if (err == ESP_OK) {
    resources &= ~kRunning;
    ap_on = false;
    mode_retry_at = 0;
    clients.store(0);
    if (sntp_on) {
      esp_netif_sntp_deinit();
      sntp_on = false;
    }
    // A previous station lease cannot restart SNTP while its radio is stopped.
    sntp_pending = false;
    if (model.hostname != applied.hostname) {
      resources &= ~(kDhcpHostname | kMdnsHostname);
      initError(InitStep::kHostname) = ESP_OK;
    }
    applied = model;
    err = configureRadio(model, step);
  }
  configured_radio = err == ESP_OK;
  if (err != ESP_OK) ESP_LOGE(kTag, "radio %s: %s", step, esp_err_to_name(err));
  return err;
}

void advanceInitialization(bool force_settings = false) {
  const int64_t now = timekeeping::monotonicMs();
  if (owns(kDnsTask)) {
    const auto error = portalDnsError();
    if (initError(InitStep::kPortalDns) != error) {
      initError(InitStep::kPortalDns) = error;
      if (failureStep() == InitStep::kNone)
        init_retry_at = 0;
      else if (init_retry_at == 0)
        init_retry_at = now + cfg::kDnsRetryMs;
      publish();
    }
  }
  if (initialized()) return;
  if (!force_settings && init_retry_at != 0 && now < init_retry_at &&
      (!settings_pending || initError(InitStep::kRadio) != ESP_OK))
    return;
  operations::Guard guard;
  if (!guard) {
    // The settings flag survives admission refusal; poll() retries with the
    // current store after the admitted operation releases its token.
    publish();
    return;
  }
  const bool retry = init_retry_at != 0 && now >= init_retry_at;
  const auto allowed = [&](InitStep step) {
    return initError(step) == ESP_OK || retry ||
           (step == InitStep::kRadio && force_settings);
  };
  const auto result = [&](InitStep step, esp_err_t err) {
    initError(step) = err;
    if (err != ESP_OK)
      ESP_LOGE(kTag, "%s: %s; retry scheduled", initStepKey(step),
               esp_err_to_name(err));
  };
  const auto run = [&](InitStep step, uint32_t owned, uint32_t dependencies,
                       auto operation) {
    if (owns(owned) || !owns(dependencies) || !allowed(step)) return;
    const auto err = operation();
    result(step, err);
    if (err == ESP_OK) resources |= owned;
  };
  // Settings can change while an early constructor is still failing. No
  // second desired network snapshot is retained; retry always consults the
  // current store.
  if (settings_pending && !started) {
    adoptModel(settings::networkSettings());
    settings_pending = false;
  }
  run(InitStep::kAntenna, kPins, 0, antennaPins);
  run(InitStep::kNetworkStack, kStack, 0, esp_netif_init);
  run(InitStep::kNetworkStack, kLoop, kStack, [] {
    const auto err = esp_event_loop_create_default();
    // The process-global loop may be borrowed. Never delete or replace it.
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
  });
  run(InitStep::kStationInterface, kStation, kStack | kLoop,
      [] { return createInterface(sta, true); });
  run(InitStep::kAccessPointInterface, kAccessPoint, kStack | kLoop,
      [] { return createInterface(ap, false); });
  run(InitStep::kAccessPointAddress, kDhcpStopped, kAccessPoint, [] {
    const auto err = esp_netif_dhcps_stop(ap);
    return err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED ? ESP_OK : err;
  });
  run(InitStep::kAccessPointAddress, kApAddress, kDhcpStopped, [] {
    esp_netif_ip_info_t address{};
    IP4_ADDR(&address.ip, cfg::kApIp[0], cfg::kApIp[1], cfg::kApIp[2],
             cfg::kApIp[3]);
    address.gw = address.ip;
    IP4_ADDR(&address.netmask, 255, 255, 255, 0);
    return esp_netif_set_ip_info(ap, &address);
  });
  run(InitStep::kAccessPointAddress, kDhcpArmed, kApAddress, [] {
    const auto err = esp_netif_dhcps_start(ap);
    return err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED ? ESP_OK : err;
  });
  run(InitStep::kWifiDriver, kDriver, kStack | kLoop, [] {
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    return esp_wifi_init(&config);
  });
  run(InitStep::kWifiConfiguration, kStorage, kDriver,
      [] { return esp_wifi_set_storage(WIFI_STORAGE_RAM); });
  run(InitStep::kWifiConfiguration, kPowerSave, kStorage, [] {
    return esp_wifi_set_ps(cfg::kWifiPowerSave ? WIFI_PS_MIN_MODEM
                                               : WIFI_PS_NONE);
  });
  run(InitStep::kWifiEvents, kWifiHandler, kLoop, [] {
    return esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event,
                                      nullptr);
  });
  run(InitStep::kWifiEvents, kIpHandler, kWifiHandler, [] {
    return esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event,
                                      nullptr);
  });
  started = owns(kRadioPrerequisites);
  if (started && (settings_pending || !configured_radio) &&
      allowed(InitStep::kRadio)) {
    const auto err = applyModel(settings::networkSettings());
    result(InitStep::kRadio, err);
    settings_pending = err != ESP_OK;
  }
  run(InitStep::kPortalDns, kDnsTask, kStack | kLoop, startPortalDns);
  run(InitStep::kMdns, kMdns, kLoop | kStation | kAccessPoint, mdns_init);
  if (!owns(kDhcpHostname | kMdnsHostname) && allowed(InitStep::kHostname))
    result(InitStep::kHostname, applyHostname());
  if (!http_ready.load() && !owns(kHttpService)) {
    initError(InitStep::kHttpService) = ESP_OK;
  } else if (owns(kMdns | kMdnsHostname) && allowed(InitStep::kHttpService)) {
    result(InitStep::kHttpService, applyHttpService());
  }
  if (failureStep() == InitStep::kNone)
    init_retry_at = 0;
  else if (retry || init_retry_at == 0)
    init_retry_at = timekeeping::monotonicMs() + cfg::kWifiRadioRetryMs;
  publish();
}
}  // namespace
esp_err_t init() {
  settings_pending = true;
  advanceInitialization();
  const auto state = status();
  return state.init_error != ESP_OK ? state.init_error
         : state.initialized        ? ESP_OK
                                    : ESP_ERR_INVALID_STATE;
}
bool healthy() {
  portENTER_CRITICAL(&mux);
  const bool value = current.initialized;
  portEXIT_CRITICAL(&mux);
  return value;
}
void setHttpReady(bool ready) { http_ready.store(ready); }
void applySettings() {
  settings_pending = true;
  advanceInitialization(true);
}
void poll() {
  advanceInitialization();
  if (!started || !configured_radio) return;
  const int64_t now = timekeeping::monotonicMs();
  const Station transition = station_event.exchange(Station::kNone);
  if (transition == Station::kGotIp) {
    policy.gotIp(now);
    sntp();
  } else if (sntp_pending && policy.state() == State::kConnected &&
             now >= sntp_retry_at) {
    sntp();
  }
  if (transition == Station::kLost) {
    cancelUserScan();
    const auto r = reason.load();
    policy.lost(r == WIFI_REASON_AUTH_FAIL ||
                    r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                    r == WIFI_REASON_HANDSHAKE_TIMEOUT,
                r == WIFI_REASON_NO_AP_FOUND, now);
  }
  const int completed = scan_done.exchange(-1);
  if (completed >= 0 && scanning) {
    wifi_ap_record_t records[cfg::kScanEntries]{};
    uint16_t count = cfg::kScanEntries;
    const auto result = esp_wifi_scan_get_ap_records(&count, records);
    // Only a successful getter frees the driver's list (SDK contract).
    if (result != ESP_OK) esp_wifi_clear_ap_list();
    const bool ok = completed == 0 && result == ESP_OK;
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
    if (user_scan)
      scan_state = ok ? UserScanState::kIdle : UserScanState::kResultFailed;
    portEXIT_CRITICAL(&mux);
    scanning = false;
    if (!user_scan) policy.scanDone(ok, found, now);
    user_scan = false;
  } else if (completed >= 0) {
    // A cancelled scan finished late: nobody reads its list.
    esp_wifi_clear_ap_list();
  }
  if (user_scan && now - user_scan_at >= cfg::kWifiScanDeadlineMs)
    cancelUserScan(UserScanState::kTimeout);
  const auto command = user_scan
                           ? Command::kNone
                           : policy.poll(now, clients.load() > 0, esp_random());
  // ap_on is the applied AP state: it follows the policy only after the
  // driver accepted the mode, a failed change is retried later.
  if (policy.ap() != ap_on && now >= mode_retry_at) {
    const bool want = policy.ap();
    const auto err = esp_wifi_set_mode(want ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err == ESP_OK) {
      ap_on = want;
      mode_retry_at = 0;
      // The driver does not document a disconnect event for the clients of
      // an AP it stops; a stale count would postpone the next fallback.
      if (!ap_on) clients.store(0);
    } else {
      mode_retry_at = now + cfg::kWifiRadioRetryMs;
      ESP_LOGE(kTag, "mode: %s; retry in %lu s", esp_err_to_name(err),
               static_cast<unsigned long>(cfg::kWifiRadioRetryMs / 1000));
    }
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
    cancelUserScan();
    esp_wifi_scan_stop();
    scanning = false;
    esp_wifi_disconnect();
  }
  if (err != ESP_OK) ESP_LOGW(kTag, "operation: %s", esp_err_to_name(err));
  // Connection scans have priority. Keep a user job queued until the radio
  // can actually start it, rather than consuming the request while busy.
  if (scanQueued() && !scanning && policy.state() != State::kConnecting &&
      policy.state() != State::kScanning) {
    wifi_scan_config_t scan{};
    scan.show_hidden = true;
    scan_done.store(-1);
    const auto started_scan = esp_wifi_scan_start(&scan, false);
    if (started_scan == ESP_OK) {
      scanning = true;
      user_scan = true;
      user_scan_at = now;
      setScanState(UserScanState::kRunning);
    } else {
      setScanState(UserScanState::kStartFailed);
      ESP_LOGW(kTag, "user scan start: %s", esp_err_to_name(started_scan));
    }
  }
  const auto snapshot = status();
  if (now - published_at >= cfg::kNetworkPublishMs ||
      snapshot.state != policy.state() || snapshot.ap != ap_on ||
      snapshot.ap_clients != clients.load()) {
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
bool ready() {
  portENTER_CRITICAL(&mux);
  const bool value = current.connected;
  portEXIT_CRITICAL(&mux);
  return value;
}
esp_err_t requestScan() {
  if (!started) return ESP_ERR_INVALID_STATE;
  portENTER_CRITICAL(&mux);
  const bool busy = userScanBusy(scan_state);
  if (!busy) scan_state = UserScanState::kQueued;
  portEXIT_CRITICAL(&mux);
  return busy ? ESP_ERR_NOT_FINISHED : ESP_OK;
}
std::size_t scanResults(AccessPoint* out, std::size_t capacity,
                        ScanStatus* status) {
  portENTER_CRITICAL(&mux);
  const auto count = std::min(capacity, result_count);
  memcpy(out, results, count * sizeof(AccessPoint));
  if (status != nullptr) {
    status->busy = userScanBusy(scan_state);
    switch (scan_state) {
      case UserScanState::kStartFailed:
        status->error_code = "scan_start";
        break;
      case UserScanState::kResultFailed:
        status->error_code = "scan_result";
        break;
      case UserScanState::kInterrupted: status->error_code = "net_lost"; break;
      case UserScanState::kTimeout: status->error_code = "timeout"; break;
      default: status->error_code = "";
    }
  }
  portEXIT_CRITICAL(&mux);
  return count;
}
}  // namespace network

// ESP-IDF documents this weak hook for applications owning clock updates.
extern "C" void sntp_sync_time(struct timeval* tv) {
  timekeeping::setTime(*tv, timekeeping::Source::kSntp);
}
