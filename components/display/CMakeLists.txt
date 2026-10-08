# Display port (ADR-003): SPI bus, panel controller driver, esp_lvgl_port.
# Managed component versions: idf_component.yml in this folder.
idf_component_register(
  SRCS
    "display_port.cpp"
    "gc9a01_panel.cpp"
  # include/ of the project for hw_profile.h (used by the public header).
  INCLUDE_DIRS
    "include"
    "../../include"
  PRIV_REQUIRES
    esp_driver_gpio
    esp_driver_spi
    esp_lcd
    freertos
    heap
    log
    espressif__esp_lvgl_port
    lvgl__lvgl
)
