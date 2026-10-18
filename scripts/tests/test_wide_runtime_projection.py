#!/usr/bin/env python3
"""Reach the final retained match through actual navigation and Presenter::bind."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="football-wide-projection-") as directory:
    scratch = Path(directory)
    (scratch / "esp_err.h").write_text(
        "#pragma once\n#include <cstdint>\n"
        "using esp_err_t = int32_t;\ninline constexpr esp_err_t ESP_OK = 0;\n"
    )
    includes = ["include", "components/football/include", "components/names/include", "components/settings/include",
                "components/ui/include", "components/ui", "components/timekeeping/include",
                "components/system/include", "components/network/include", "components/images/include",
                "components/geometry/include", "components/hardware/include", "components/files/include",
                "components/json/include"]
    sources = ["components/football/football_model.cpp", "components/football/match_store.cpp",
               "components/football/matchday.cpp", "components/football/store_views.cpp",
               "components/football/screen_selection.cpp", "components/ui/navigation_policy.cpp",
               "components/ui/screen_sequence.cpp", "components/ui/football_presenter.cpp",
               "components/settings/settings_model.cpp", "components/settings/ui_config.cpp", "components/settings/screen_policy.cpp",
               "components/timekeeping/civil_time.cpp", "components/network/wifi_policy.cpp",
               "scripts/tests/fixtures/wide_runtime_projection_host.cpp"]
    command = ["clang++", "-std=c++20", "-g", "-fsanitize=address,undefined",
               "-fno-omit-frame-pointer", "-I", str(scratch)]
    for include in includes:
        command += ["-I", str(root / include)]
    command += [str(root / source) for source in sources]
    binary = scratch / "projection"
    subprocess.run([*command, "-o", str(binary)], check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
