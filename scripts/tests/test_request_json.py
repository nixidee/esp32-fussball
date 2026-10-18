#!/usr/bin/env python3
"""Replay actual command codecs without ESP hardware or HTTP mocks."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="football-request-json-") as temp:
    binary = Path(temp) / "requests"
    command = ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-ffunction-sections"]
    for folder in ["include", "components/json/include", "components/web", "components/football/include", "components/settings/include", "components/timekeeping/include"]:
        command += ["-I", str(root / folder)]
    command += [str(root / file) for file in ["components/json/json_stream.cpp", "components/web/request_json.cpp", "components/settings/settings_model.cpp", "components/timekeeping/civil_time.cpp", "components/football/football_model.cpp", "components/football/demo_scenarios.cpp", "components/football/match_store.cpp", "scripts/tests/request_json_host.cpp"]]
    command += ["-Wl,-dead_strip", "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
