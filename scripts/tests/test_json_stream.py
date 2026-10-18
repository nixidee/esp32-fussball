#!/usr/bin/env python3
"""Replay the actual bounded JSON protocol with ASan/UBSan, no firmware build."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="football-json-") as directory:
    binary = Path(directory) / "json_stream"
    command = ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-I", str(root / "include"),
               "-I", str(root / "components/json/include"),
               "-I", str(root / "components/web"),
               "-I", str(root / "components/ui/include"),
               "-I", str(root / "components/settings/include"),
               str(root / "components/json/json_stream.cpp"),
               str(root / "components/web/command_json.cpp"),
               str(root / "scripts/tests/json_stream_host.cpp"), "-o", str(binary)]
    subprocess.run(command, check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
