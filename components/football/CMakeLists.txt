idf_component_register(SRCS "football_model.cpp" "source_index.cpp" "fixture_retention.cpp" "provider_client.cpp" "provider_adapter.cpp" "provider_readers.cpp" "provider_selection.cpp" "provider_coverage.cpp" "demo_scenarios.cpp" "football_service.cpp" "request_budget.cpp" "budget_ledger.cpp" "match_store.cpp" "store_mappers.cpp" "request_scheduler.cpp" "provider_plan.cpp" "store_views.cpp" "matchday.cpp" "screen_selection.cpp" "schedule_coverage.cpp" "original_names_channel.cpp"
  INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "../../include"
  REQUIRES json settings network timekeeping events
  PRIV_REQUIRES esp_http_client tcp_transport mbedtls esp_timer freertos nvs_flash system lwip)
