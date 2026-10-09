# The P1.8 budget probe is compiled only when CONFIG_FUSSBALL_BUDGET_PROBE is
# set (env xiao_esp32c6_gc9a01_budget, sdkconfig.defaults.budget).
set(app_sources main.cpp)
if(CONFIG_FUSSBALL_BUDGET_PROBE)
  list(APPEND app_sources budget_probe.cpp)
endif()

idf_component_register(
  SRCS
    ${app_sources}
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
    geometry
    heap
    log
    ui
)

# Probe-only IDF components. Config options are not available while component
# requirements are collected, so they are linked after the registration.
if(CONFIG_FUSSBALL_BUDGET_PROBE)
  foreach(probe_component
      esp_event esp_http_client esp_http_server esp_netif esp_timer esp_wifi mbedtls nvs_flash)
    idf_component_get_property(probe_lib ${probe_component} COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${probe_lib})
  endforeach()

  # The ESP32-C6 ROM linker script also defines jd_prepare and jd_decomp (an
  # older TJpgDec with a different JDEC layout). The ROM address wins over the
  # LVGL TJPGD objects, so LVGL's decoder would run ROM code on its own JDEC
  # and crash. Give the LVGL functions their own names.
  idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
  target_compile_definitions(${lvgl_lib} PRIVATE
    jd_prepare=lvgl_jd_prepare
    jd_decomp=lvgl_jd_decomp)
endif()
