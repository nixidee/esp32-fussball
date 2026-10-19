#!/usr/bin/env python3
"""Exact season proof ownership, deduplication, joining and invalidation."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="football-source-store-") as temporary:
    binary = Path(temporary) / "source-store"
    command = [
        "clang++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    ]
    for component in ("football", "timekeeping", "json", "settings", "network"):
        command += ["-I" + str(ROOT / "components" / component / "include")]
    command += ["-I" + str(ROOT / "include")]
    for source in (
        "scripts/tests/fixtures/source_coverage_host.cpp",
        "components/football/schedule_coverage.cpp",
        "components/football/budget_ledger.cpp",
        "components/settings/settings_record.cpp",
        "components/football/match_store.cpp",
        "components/football/source_index.cpp",
        "components/football/store_mappers.cpp",
        "components/football/store_views.cpp",
        "components/football/screen_selection.cpp",
        "components/football/demo_scenarios.cpp",
        "components/football/football_model.cpp",
        "components/settings/settings_model.cpp",
        "components/settings/screen_policy.cpp",
        "components/network/wifi_policy.cpp",
        "components/timekeeping/civil_time.cpp",
    ):
        command.append(str(ROOT / source))
    subprocess.run([*command, "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
