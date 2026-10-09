# Pure settings model and record format (docs/CONFIGURATION.md); no ESP-IDF
# includes, host-tested. app_config.h is a public dependency.
idf_component_register(
  SRCS
    "settings_model.cpp"
    "settings_record.cpp"
  INCLUDE_DIRS
    "include"
    "../../include"
)
