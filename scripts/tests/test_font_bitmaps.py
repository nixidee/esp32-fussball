#!/usr/bin/env python3
"""Check every original glyph through actual installed LVGL and the font callback.

Goldens are the pre-compression raw bitmap hashes, not a second rasterizer.
The small Python stream reader reconstructs native plain-font fixtures; their
hashes must match the original before any callback comparison is accepted.
"""
from pathlib import Path
import hashlib
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
lvgl = root / "managed_components/lvgl__lvgl"
fonts = root / "components/ui/fonts"
raw_hashes = {
    "text_12": "8e04dc4cc90503521100278de5a62014c69db6b2eeca3b70b04adba4ce86f4d9",
    "text_14": "e60abf7f705ce689d12ed62dfda297fa7c49886ab129d85b70932407568cffb0",
    "text_18": "e20f2471def0692acd65ecca4813fc75d1d84fa4b4c1905043094becbb4cf5a5",
    "score_48": "2b0bca0d9706168992cc6dda6a8c3d80d24b775fc8b7ef25f2266cd7ed9833e9",
}
metrics_hashes = {
    "text_12": "b0a82d9a43d6e7c1c7efba2a6171d46824b186c4225a8427f6f83de7ac8d99df",
    "text_14": "7473f313be21614f09decb8af2a1a5b8c67c57216a326207bb5869b53adc1ecd",
    "text_18": "9f5582bfe6cac4fc2e8fb1d7a3612b0c9935d23b670e2d112f9f59786f6b552a",
    "score_48": "37e2ddf765f77e02fc2e0cbaa985e2183b299a23241418a8d5afebe7a654c244",
}


def unpack(data, count):
    bits = iter((byte >> shift) & 1 for byte in data for shift in range(7, -1, -1))

    def take(n):
        value = 0
        for _ in range(n):
            value = value * 2 + next(bits)
        return value

    state, previous, repeats, pixels = "single", None, 0, []
    for _ in range(count):
        value = previous
        if state == "single":
            value = take(4)
            if value == previous:
                state, repeats = "repeated", 0
        elif state == "repeated":
            same = take(1)
            repeats += 1
            if same and repeats == 11:
                repeats = take(6)
                state = "counter" if repeats else "single"
                if not repeats:
                    value = take(4)
            elif not same:
                value, state = take(4), "single"
        else:
            repeats -= 1
            if not repeats:
                value, state = take(4), "single"
        pixels.append(value)
        previous = value
    return bytes((pixels[i] << 4) | (pixels[i+1] if i+1 < count else 0)
                 for i in range(0, count, 2))


with tempfile.TemporaryDirectory(prefix="font-bitmaps-") as directory:
    folder = Path(directory)
    (folder / "lv_conf.h").write_text("""
#define LV_CONF_H
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_DRAW_BUF_STRIDE_ALIGN 4
""")
    sources, declarations, case_rows = [], [], []
    for role, digest in raw_hashes.items():
        name = "ui_font_" + role
        source = (fonts / (name + ".c")).read_text()
        begin, end = source.index("glyph_bitmap[]"), source.index("glyph_dsc[]")
        encoded = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]+)", source[begin:end]))
        desc_begin, desc_end = source.index("{", end), source.index("unicode_list[]")
        descriptors = [{key: int(value) for key, value in re.findall(
            r"\.(bitmap_index|adv_w|box_w|box_h|ofs_x|ofs_y)\s*=\s*(-?\d+)", item)}
            for item in re.findall(r"\{([^}]+)\}", source[desc_begin:desc_end])]
        plain = bytearray()
        offsets = []
        for i, desc in enumerate(descriptors):
            offsets.append(len(plain))
            next_offset = descriptors[i+1]["bitmap_index"] if i+1 < len(descriptors) else len(encoded)
            plain.extend(unpack(encoded[desc["bitmap_index"]:next_offset], desc["box_w"]*desc["box_h"]))
        assert hashlib.sha256(plain).hexdigest() == digest, role + " original pixels changed"
        replacement = "glyph_bitmap[] = {" + ",".join(str(v) for v in plain) + "};\nstatic const lv_font_fmt_txt_glyph_dsc_t "
        text = source[:begin] + replacement + source[end:]
        cursor = iter(offsets)
        text = re.sub(r"\.bitmap_index\s*=\s*\d+", lambda _: ".bitmap_index="+str(next(cursor)), text)
        text = re.sub(r"static const ui_font_rle_dsc_t font_dsc = .*?;", "static const lv_font_fmt_txt_dsc_t font_dsc = {.glyph_bitmap=glyph_bitmap,.glyph_dsc=glyph_dsc,.cmaps=cmaps,.cmap_num=1,.bpp=4,.bitmap_format=LV_FONT_FMT_TXT_PLAIN};", text, flags=re.S)
        text = text.replace(".get_glyph_bitmap = ui_font_rle_bitmap", ".get_glyph_bitmap = lv_font_get_bitmap_fmt_txt")
        text = text.replace("const lv_font_t " + name, "const lv_font_t plain_" + name)
        target = folder / ("plain_" + name + ".c")
        target.write_text(text)
        sources += [fonts / (name + ".c"), target]
        declarations.append(f"extern const lv_font_t {name}, plain_{name};")
        first = int(re.search(r"\.range_start\s*=\s*(\d+)", source)[1])
        points = [int(v)+first for v in re.findall(r"\d+", re.search(r"unicode_list\[\]\s*=\s*\{([^}]+)", source)[1])]
        metrics = [tuple(desc[key] for key in ("adv_w", "box_w", "box_h", "ofs_x", "ofs_y"))
                   for desc in descriptors]
        metadata = [int(re.search(r"\."+key+r"\s*=\s*(\d+)", source)[1])
                    for key in ("line_height", "base_line", "cap_height", "x_height")]
        assert hashlib.sha256(repr((points, metrics, metadata)).encode()).hexdigest() == metrics_hashes[role], role + " original metrics changed"
        declarations.append(f"static const uint32_t points_{role}[]={{"+",".join(map(str, points))+"};")
        case_rows.append(f"{{&{name},&plain_{name},points_{role},{len(points)}}}")
    (folder / "font_cases.h").write_text('#include "ui_font_rle.h"\n'+"\n".join(declarations)+"\nstatic const struct {const lv_font_t *font,*plain; const uint32_t *codepoints;unsigned count;} cases[]={"+",".join(case_rows)+"};\n")
    sources += [fonts / "ui_font_rle.c", root / "scripts/tests/fixtures/font_bitmaps_host.c",
                lvgl / "src/font/lv_font.c", lvgl / "src/font/fmt_txt/lv_font_fmt_txt.c",
                lvgl / "src/misc/lv_utils.c", lvgl / "src/stdlib/clib/lv_string_clib.c"]
    binary = folder / "check"
    command = ["clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-ffunction-sections", "-fdata-sections", "-Wl,-dead_strip",
               "-DLV_CONF_INCLUDE_SIMPLE", "-I"+str(folder), "-I"+str(lvgl), "-I"+str(fonts)]
    subprocess.run(command + list(map(str, sources)) + ["-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
