# Views (docs/ARCHITECTURE.md): render view models with LVGL, no logic.
# LVGL version: components/display/idf_component.yml.
idf_component_register(
  SRCS
    "boot_screen.cpp"
    "football_screen.cpp"
    "ui_controller.cpp"
    "navigation_policy.cpp"
    "input_policy.cpp"
    "fonts/ui_font_text_10.c"
    "fonts/ui_font_text_14.c"
    "fonts/ui_font_text_18.c"
    "fonts/ui_font_score_28.c"
  INCLUDE_DIRS
    "include"
  PRIV_INCLUDE_DIRS
    "../../include"
  PRIV_REQUIRES
    geometry
    lvgl__lvgl
    core
    display
    football
    images
    network
    settings
)
