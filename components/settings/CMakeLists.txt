# Configuration owns pure models/codecs and the NVS application owner.
idf_component_register(
  SRCS
    "app_config_model.cpp"
    "app_config_record.cpp"
    "settings_model.cpp"
    "settings_record.cpp"
    "screen_policy.cpp"
    "ui_config.cpp"
    "screen_record.cpp"
    "runtime_config.cpp"
    "settings_store.cpp"
    "settings_test.cpp"
  INCLUDE_DIRS
    "include"
    "../../include"
  REQUIRES
    timekeeping
    events
  PRIV_REQUIRES nvs_flash freertos log
)
