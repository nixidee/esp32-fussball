idf_component_register(
  SRCS
    "main.cpp"
  # Project root for the hardware profiles (boards/, displays/, targets/)
  # and include/ for project-wide headers (ADR-004).
  PRIV_INCLUDE_DIRS
    ".."
    "../include"
  PRIV_REQUIRES
    core
    display
    esp_app_format
    esp_driver_gpio
    esp_hw_support
    events
    freertos
    geometry
    heap
    log
    settings
    ui
)
