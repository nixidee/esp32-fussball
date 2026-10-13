// Included by LVGL's configuration (CONFIG_LV_FONT_CUSTOM_INCLUDE): the
// default font of every LVGL object is the 14 px text role.
#pragma once
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(ui_font_text_14)
// lv_conf_internal.h defines a built-in fallback before including this file.
#undef LV_FONT_DEFAULT
#define LV_FONT_DEFAULT &ui_font_text_14
