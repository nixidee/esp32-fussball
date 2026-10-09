# Time-zone lookup and civil-time window rule of the time service
# (docs/NETWORK.md "Time"); no ESP-IDF includes, host-tested. app_config.h is
# a public dependency.
idf_component_register(
  SRCS
    "civil_time.cpp"
  INCLUDE_DIRS
    "include"
    "../../include"
)
