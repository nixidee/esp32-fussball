idf_component_register(SRCS "wifi_policy.cpp" "network_service.cpp"
  INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "../../include" "../.."
  REQUIRES settings core
  PRIV_REQUIRES esp_wifi esp_netif esp_event esp_driver_gpio esp_hw_support
    esp_timer lwip freertos espressif__mdns)
