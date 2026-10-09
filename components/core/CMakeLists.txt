# Core services (docs/ARCHITECTURE.md). Currently the settings store on NVS;
# the file service, time, events and health follow.
idf_component_register(
  SRCS
    "settings_store.cpp"
    "settings_test.cpp"
  INCLUDE_DIRS
    "include"
  REQUIRES
    settings
  PRIV_REQUIRES
    freertos
    log
    nvs_flash
)
