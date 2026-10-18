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

# LVGL's default font is the 14 px text role of the ui component
# (sdkconfig.defaults: LV_USE_CUSTOM_FONT_DEFAULT). LVGL includes the header
# named by LV_FONT_CUSTOM_INCLUDE from its own configuration, so its folder is
# public on the LVGL library and reaches every user of lvgl.h.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib} PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/../components/ui/lvgl_font")
