#!/usr/bin/env python3
"""Exact resolved identity and non-evicting source lifetime, actual production TU."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="football-source-index-") as temp:
    binary = Path(temp) / "source-index"
    subprocess.run([
        "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I" + str(ROOT / "include"),
        "-I" + str(ROOT / "components/football/include"),
        str(ROOT / "scripts/tests/fixtures/source_index_host.cpp"),
        str(ROOT / "components/football/source_index.cpp"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
