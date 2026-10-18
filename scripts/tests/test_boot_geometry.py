#!/usr/bin/env python3
"""Exercise colour-test geometry in actual LVGL, with software drawing off.

No display buffer, flush callback, timer dispatch or host pixels. LVGL supplies
the object styles, wrapped label heights and positions; SafeArea checks the
complete content. The existing installed CMake/Ninja/LVGL are used unchanged.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
packages = Path.home() / ".platformio/packages"
cmake = shutil.which("cmake") or str(packages / "tool-cmake/bin/cmake")
ninja = shutil.which("ninja") or str(packages / "tool-ninja/ninja")


def run(arguments):
    result = subprocess.run(arguments, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        print("\n".join(result.stdout.splitlines()[-45:]))
        result.check_returncode()


with tempfile.TemporaryDirectory(prefix="boot-geometry-") as directory:
    folder = Path(directory)
    host = {
        "LV_CONF_H": "",
        "LV_USE_OS": "LV_OS_NONE",
        "LV_USE_STDLIB_MALLOC": "LV_STDLIB_CLIB",
        "LV_USE_STDLIB_STRING": "LV_STDLIB_CLIB",
        "LV_USE_STDLIB_SPRINTF": "LV_STDLIB_CLIB",
        "LV_USE_DRAW_SW": "0",  # geometry only; no software renderer
        "LV_FONT_MONTSERRAT_14": "0",
        "LV_FONT_CUSTOM_DECLARE": "LV_FONT_DECLARE(ui_font_text_14)",
        "LV_FONT_DEFAULT": "&ui_font_text_14",
        "LV_USE_FONT_COMPRESSED": "0",
    }
    # Preserve actual disabled theme/layout/widgets rather than another list.
    for name in re.findall(r"^# CONFIG_(LV_USE_[A-Z0-9_]+) is not set$",
                           (root / "sdkconfig.defaults").read_text(), re.M):
        host[name] = "0"
    (folder / "lv_conf.h").write_text("\n".join(
        "#define " + key + " " + value for key, value in host.items()) + "\n")
    (folder / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.16)
project(boot_geometry LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
foreach(option LV_FETCH_DEPENDENCIES LV_USE_FIND_PACKAGE LV_USE_PKG_CONFIG
               CONFIG_LV_BUILD_EXAMPLES CONFIG_LV_BUILD_DEMOS
               CONFIG_LV_USE_THORVG CONFIG_LV_USE_THORVG_INTERNAL LV_BUILD_INSTALL)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(${ROOT}/managed_components/lvgl__lvgl lvgl)
add_executable(boot_geometry ${ROOT}/scripts/tests/fixtures/boot_geometry_host.cpp
  ${ROOT}/components/ui/boot_screen.cpp ${ROOT}/components/geometry/safe_area.cpp
  ${ROOT}/components/ui/fonts/ui_font_text_14.c ${ROOT}/components/ui/fonts/ui_font_rle.c)
target_include_directories(boot_geometry PRIVATE ${ROOT}/include
  ${ROOT}/components/ui/include ${ROOT}/components/ui/fonts ${ROOT}/components/geometry/include)
target_link_libraries(boot_geometry PRIVATE lvgl)
''')
    build = folder / "build"
    run([cmake, "-S", str(folder), "-B", str(build), "-G", "Ninja",
         "-DCMAKE_MAKE_PROGRAM=" + ninja, "-DROOT=" + str(root)])
    run([cmake, "--build", str(build), "--target", "boot_geometry", "-j", "4"])
    subprocess.run([str(build / "boot_geometry")], check=True)
