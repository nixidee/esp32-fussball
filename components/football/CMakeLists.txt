idf_component_register(SRCS "football_model.cpp" "provider_client.cpp" "provider_json.cpp" "provider_coverage.cpp" "demo_scenarios.cpp" "football_service.cpp" "request_budget.cpp" "budget_ledger.cpp" "match_store.cpp" "store_mappers.cpp" "request_scheduler.cpp" "provider_plan.cpp" "store_views.cpp" "football_files.cpp" "matchday.cpp" "screen_selection.cpp" "schedule_coverage.cpp"
  INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "../../include"
  REQUIRES settings core network bblanchon__arduinojson
  PRIV_REQUIRES images esp_http_client tcp_transport mbedtls esp_timer freertos nvs_flash system lwip)
