# Portable admission policy and the state-notification bus own one contract.
idf_component_register(
  SRCS
    "event_admission.cpp"
    "event_bus.cpp"
    "event_test.cpp"
  INCLUDE_DIRS
    "include"
    "../../include"
  PRIV_REQUIRES esp_event freertos log
)
