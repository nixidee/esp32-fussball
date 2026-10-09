# Core services (docs/ARCHITECTURE.md). Currently the settings store on NVS,
# the event bus, the file service (LittleFS) and the time service; health
# follows.
# Managed component versions: idf_component.yml in this folder.
idf_component_register(
  SRCS
    "event_bus.cpp"
    "event_test.cpp"
    "file_service.cpp"
    "file_test.cpp"
    "settings_store.cpp"
    "settings_test.cpp"
    "time_service.cpp"
    "time_test.cpp"
  INCLUDE_DIRS
    "include"
  REQUIRES
    events
    files
    settings
    timekeeping
  PRIV_REQUIRES
    esp_event
    esp_partition
    esp_timer
    freertos
    heap
    joltwallet__littlefs
    log
    nvs_flash
    vfs
)
