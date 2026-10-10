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
    files
    freertos
    geometry
    heap
    log
    settings
    timekeeping
    ui
    network
    football
    images
    system
    web
)

# C6 ROM uses an incompatible older TJpgDec ABI. Rename only the LVGL
# library's symbols; managed sources and the installed SDK stay unchanged.
if(CONFIG_LV_USE_TJPGD)
  idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
  target_compile_definitions(${lvgl_lib} PRIVATE
    jd_prepare=lvgl_jd_prepare jd_decomp=lvgl_jd_decomp)
endif()
