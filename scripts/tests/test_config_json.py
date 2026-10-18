#!/usr/bin/env python3
"""Replay complete aggregate schemas against actual settings/JSON codecs."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="football-config-json-") as temp:
    command = ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for folder in ["include", "components/json/include", "components/web", "components/settings/include", "components/football/include", "components/timekeeping/include"]:
        command += ["-I", str(root / folder)]
    for file in ["components/json/json_stream.cpp", "components/settings/settings_model.cpp", "components/settings/screen_policy.cpp", "components/settings/ui_config.cpp", "components/timekeeping/civil_time.cpp", "components/football/football_model.cpp", "components/web/settings_json.cpp", "components/web/screen_json.cpp", "components/web/config_json.cpp", "scripts/tests/config_json_host.cpp"]:
        command += [str(root / file)]
    binary = Path(temp) / "config"
    subprocess.run(command + ["-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
