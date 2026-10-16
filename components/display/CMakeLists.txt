# Display port (ADR-003): SPI bus, panel controller driver, esp_lvgl_port.
# Managed component versions: idf_component.yml in this folder.
idf_component_register(
  SRCS
    "display_port.cpp"
    "gc9a01_panel.cpp"
    "jpeg_validation.cpp"
    "jpeg_observer.cpp"
  # include/ of the project for hw_profile.h (used by the public header).
  INCLUDE_DIRS
    "include"
    "../../include"
  PRIV_INCLUDE_DIRS "../.."
  PRIV_REQUIRES
    esp_driver_gpio
    esp_driver_ledc
    esp_driver_spi
    esp_lcd
    esp_system
    esp_timer
    freertos
    heap
    log
    espressif__esp_lvgl_port
    lvgl__lvgl
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  idf_component_get_property(JPEG_OBSERVER_LVGL_ROOT lvgl__lvgl COMPONENT_DIR)
  include("${CMAKE_CURRENT_LIST_DIR}/../../scripts/jpeg_observer_guard.cmake")
endif()
