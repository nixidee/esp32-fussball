# Civil-time rules and the applied clock owner; no configuration dependency.
idf_component_register(
  SRCS
    "civil_time.cpp"
    "time_service.cpp"
    "time_test.cpp"
  INCLUDE_DIRS
    "include"
    "../../include"
  REQUIRES events
  PRIV_REQUIRES esp_timer freertos heap log
)
