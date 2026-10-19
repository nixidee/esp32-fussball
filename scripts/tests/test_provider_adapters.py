#!/usr/bin/env python3
"""Actual sporting adapter/model replay; no network, DOM or firmware build."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="football-provider-adapters-") as directory:
    binary = Path(directory) / "adapters"
    command = ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for path in ("include", "components/football/include", "components/json/include", "components/timekeeping/include", "components/settings/include", "components/network/include"):
        command += ["-I", str(root / path)]
    for path in ("components/football/source_index.cpp", "components/football/fixture_retention.cpp", "components/football/provider_adapter.cpp", "components/football/provider_readers.cpp", "components/football/provider_plan.cpp",
                 "components/football/request_scheduler.cpp", "components/football/screen_selection.cpp",
                 "components/football/budget_ledger.cpp", "components/settings/settings_model.cpp",
                 "components/settings/screen_policy.cpp", "components/settings/settings_record.cpp",
                 "components/network/wifi_policy.cpp", "components/football/store_mappers.cpp",
                 "components/football/football_model.cpp", "components/football/match_store.cpp",
                 "components/football/matchday.cpp", "components/football/store_views.cpp",
                 "components/json/json_stream.cpp", "components/timekeeping/civil_time.cpp",
                 "scripts/tests/fixtures/provider_adapters_host.cpp"):
        command += [str(root / path)]
    subprocess.run([*command, "-o", str(binary)], check=True, cwd=root)
    subprocess.run([str(binary), str(root / "aidocs/Reviews/evidence_2026-10-17/provider-public")], check=True, cwd=root)
