#!/usr/bin/env python3
"""Actual decoder, HTTP/FIFO and controller input/reload/poll revision ownership.

SDK/network/render adapters are seams. No alternate command arbitration or
renderer is implemented: the production functions are included unchanged.
"""
from pathlib import Path
import subprocess
import tempfile
from test_runtime_readiness import function

ROOT = Path(__file__).resolve().parents[2]
web = (ROOT / "components/web/web_service.cpp").read_text()
controller = (ROOT / "components/ui/ui_controller.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="football-display-commands-") as directory:
    scratch = Path(directory)
    (scratch / "esp_err.h").write_text(
        "#pragma once\nusing esp_err_t=int;\n"
        "inline constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_ARG=0x102,"
        "ESP_ERR_INVALID_STATE=0x103,ESP_ERR_TIMEOUT=0x107,"
        "ESP_ERR_NOT_FINISHED=0x109;\n")
    (scratch / "display_http.inc").write_text(function(web, "esp_err_t postDisplay("))
    (scratch / "command_body.inc").write_text(
        "template <class T, class Decode>\n" + function(web, "bool readCommand("))
    (scratch / "display_submit.inc").write_text(function(controller, "esp_err_t submit("))
    (scratch / "display_poll.inc").write_text(function(controller, "void poll()"))
    (scratch / "display_input.inc").write_text(function(controller, "void input("))
    (scratch / "display_reload.inc").write_text(function(controller, "void reloadSettings(int64_t now, settings::screen_policy::Phase phase) {"))
    (scratch / "display_same_navigation.inc").write_text(function((ROOT / "components/settings/ui_config.cpp").read_text(), "bool sameNavigation("))
    command = ["clang++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-I", str(scratch), "-I", str(ROOT / "include"),
               "-I", str(ROOT / "components/web"), "-I", str(ROOT / "components/ui")]
    for path in (ROOT / "components").glob("*/include"):
        command += ["-I", str(path)]
    command += [str(ROOT / "components/json/json_stream.cpp"),
                str(ROOT / "components/web/command_json.cpp"),
                str(ROOT / "components/ui/input_policy.cpp"),
                str(ROOT / "scripts/tests/fixtures/display_commands_host.cpp"),
                "-o", str(scratch / "check")]
    subprocess.run(command, check=True)
    subprocess.run([str(scratch / "check")], check=True)
