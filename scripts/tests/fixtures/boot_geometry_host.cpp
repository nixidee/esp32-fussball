// Actual LVGL widget geometry only. Software drawing disabled; no display
// buffer, flush callback, timer dispatch or pixels are produced.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "app_config.h"
#include "boot_screen.h"
#include "lvgl.h"
#include "safe_area.h"
int main() {
  lv_init();
  unsigned cases = 0;
  for (int side : {240, 466})
    for (auto shape : {hw::DisplayShape::kRound, hw::DisplayShape::kRect}) {
      auto* display = lv_display_create(side, side);
      assert(display);
      lv_display_set_default(display);
      geometry::SafeArea area(side, side, shape,
                              side / cfg::kContentMarginDivisor);
      for (const char* detail :
           {"240 x 240 · SPI",
            "Display colour test: Titel und längere Detailzeile"}) {
        ui::BootScreenModel model{"Fussball", detail, "v0.9.0"};
        assert(ui::showBootScreen(model, area));
        auto* screen = lv_screen_active();
        assert(lv_obj_get_child_count(screen) == 3);
        auto* text = lv_obj_get_child(screen, 2);
        assert(lv_obj_get_child_count(text) == 3);
        lv_area_t bounds;
        lv_obj_get_coords(text, &bounds);
        assert(area.contains({bounds.x1, bounds.y1, lv_area_get_width(&bounds),
                              lv_area_get_height(&bounds)}));
        int next = 0;
        for (int i = 0; i < 3; ++i) {
          auto* label = lv_obj_get_child(text, i);
          assert(lv_obj_get_y(label) == next);
          next += lv_obj_get_height(label) + side / cfg::kBootRingGapDivisor;
          assert(lv_obj_get_style_text_font(label, LV_PART_MAIN) ==
                 LV_FONT_DEFAULT);
          assert(lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN),
                             lv_color_hex(i == 2 ? 0xff : 0xffffff)));
          assert(std::strcmp(lv_label_get_text(label),
                             i == 0   ? model.title
                             : i == 1 ? model.detail
                                      : model.version) == 0);
        }
        ++cases;
      }
      const std::string impossible(1000, 'W');
      assert(
          !ui::showBootScreen({impossible.c_str(), "Detail", "Version"}, area));
      ++cases;
      lv_display_delete(display);
    }
  lv_deinit();
  std::printf(
      "PASS %u actual LVGL colour-test geometry/style/wrap/rejection cases; "
      "draw software disabled, no pixels\n",
      cases);
}
