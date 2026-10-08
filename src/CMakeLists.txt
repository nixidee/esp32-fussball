idf_component_register(
  SRCS
    "main.cpp"
  # Project root for the hardware profiles (boards/, displays/, targets/)
  # and include/ for project-wide headers (ADR-004).
  PRIV_INCLUDE_DIRS
    ".."
    "../include"
  PRIV_REQUIRES
    display
    esp_app_format
    esp_driver_gpio
    esp_hw_support
    freertos
    heap
    log
    ui
)
