# Event catalog and queue admission of the event bus (docs/ARCHITECTURE.md);
# no ESP-IDF includes, host-tested. app_config.h is a public dependency.
idf_component_register(
  SRCS
    "event_admission.cpp"
  INCLUDE_DIRS
    "include"
    "../../include"
)
