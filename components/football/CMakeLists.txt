idf_component_register(SRCS "football_model.cpp" "provider_client.cpp" "provider_mappers.cpp" "football_service.cpp" "request_budget.cpp"
  INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "../../include"
  REQUIRES settings core network bblanchon__arduinojson
  PRIV_REQUIRES esp_http_client tcp_transport mbedtls esp_timer freertos nvs_flash system lwip)
