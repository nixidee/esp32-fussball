# Core services (docs/ARCHITECTURE.md). Currently the settings store on NVS
# and the event bus; the file service, time and health follow.
idf_component_register(
  SRCS
    "event_bus.cpp"
    "event_test.cpp"
    "settings_store.cpp"
    "settings_test.cpp"
  INCLUDE_DIRS
    "include"
  REQUIRES
    events
    settings
  PRIV_REQUIRES
    esp_event
    freertos
    log
    nvs_flash
)
