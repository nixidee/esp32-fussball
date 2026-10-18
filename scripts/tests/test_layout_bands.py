#!/usr/bin/env python3
"""Run actual layout functions/fit rules against SafeArea, without drawing pixels.

Glyph advances/line heights come from the compiled font sources. The SDK's
advance rounding is checked explicitly; no alternate layout is implemented.
This checks strings and rectangles, not LCD legibility or LVGL transfers.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "components/ui/football_screen.cpp").read_text()

def between(start, end):
    a = source.index(start)
    return source[a:source.index(end, a)]

metrics = []
for name in ("text_12", "text_14", "text_18", "score_48"):
    font = (root / f"components/ui/fonts/ui_font_{name}.c").read_text()
    descriptors = font[font.index("glyph_dsc[]") : font.index("unicode_list[]")]
    advances = [int(v) for v in re.findall(r"\.adv_w\s*=\s*(\d+)", descriptors)]
    codepoints = [int(v) for v in re.findall(r"\d+", re.search(
        r"unicode_list\[\]\s*=\s*\{([^}]+)", font)[1])]
    first = int(re.search(r"\.range_start\s*=\s*(\d+)", font)[1])
    assert len(advances) == len(codepoints) + 1
    pairs = ",".join("{%d,%d}" % (first + cp, (advance + 8) >> 4)
                     for cp, advance in zip(codepoints, advances[1:]))
    height = int(re.search(r"\.line_height\s*=\s*(\d+)", font)[1])
    metrics.append("{%d,{%s}}" % (height, pairs))
sdk = (root / "managed_components/lvgl__lvgl/src/font/fmt_txt/lv_font_fmt_txt.c").read_text()
assert "(adv_w + (1 << 3)) >> 4" in sdk

prefix = '''#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include "football_screen.h"
using namespace ui;
using lv_opa_t=uint8_t;
constexpr lv_opa_t LV_OPA_COVER=255;
struct FontMetric {int line_height;std::map<uint32_t,int> advances;};
'''
prefix += "FontMetric metrics[] = {" + ",\n".join(metrics) + "};\n"
prefix += '''const FontMetric* const kFonts[]={&metrics[0],&metrics[1],&metrics[2],&metrics[3]};
constexpr uint8_t kScoreFont=3;
constexpr char kEllipsis[]="…";
int lv_font_get_glyph_width(const FontMetric* f,uint32_t cp,uint32_t){
 const auto it=f->advances.find(cp);return it==f->advances.end()?f->advances.at('?'):it->second;
}
'''
prefix += between("enum class Kind", "const lv_font_t* const kFonts[]")
prefix += between("uint32_t decode(", "lv_area_t arcArea(")
prefix += between("struct Frame {", "// Largest compiled text font")
prefix += "\nvoid push(Item item){if(item_count>=kItems){overflow=true;return;}items[item_count++]=item;}\n"
prefix += between("uint8_t fontFor(", "// Arc on the rim")
prefix += between("// IP chip:", "void rim()")

with tempfile.TemporaryDirectory(prefix="layout-bands-") as directory:
    folder = Path(directory)
    (folder / "layout.inc").write_text(prefix)
    binary = folder / "check"
    command = ["clang++", "-std=c++20", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-variable", "-fsanitize=address,undefined",
               "-fno-omit-frame-pointer", "-I" + str(folder)]
    command += ["-I" + str(root / path) for path in (
        "include", "components/ui/include", "components/settings/include",
        "components/geometry/include", "components/timekeeping/include")]
    command += [str(root / "scripts/tests/fixtures/layout_bands_host.cpp"),
                str(root / "components/geometry/safe_area.cpp"), "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
