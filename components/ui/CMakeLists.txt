# Views (docs/ARCHITECTURE.md): render view models with LVGL, no logic.
# LVGL version: components/display/idf_component.yml.
idf_component_register(
  SRCS
    "boot_screen.cpp"
  INCLUDE_DIRS
    "include"
  PRIV_INCLUDE_DIRS
    "../../include"
  PRIV_REQUIRES
    geometry
    lvgl__lvgl
)
