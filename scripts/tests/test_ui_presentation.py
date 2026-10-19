#!/usr/bin/env python3
"""Actual canonical-store/Presenter projection, without a renderer."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="football-presentation-") as directory:
    scratch=Path(directory)
    (scratch / "esp_err.h").write_text("#pragma once\nusing esp_err_t=int;\ninline constexpr esp_err_t ESP_OK=0;\n")
    command=["clang++","-std=c++20","-g","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-fno-omit-frame-pointer","-I",str(scratch)]
    for include in ["include","components/football/include", "components/names/include","components/settings/include","components/ui/include","components/ui","components/system/include","components/network/include","components/images/include","components/geometry/include","components/timekeeping/include","components/files/include","components/json/include"]:
        command += ["-I",str(root/include)]
    for source in ["components/football/football_model.cpp","components/football/source_index.cpp","components/football/match_store.cpp","components/football/matchday.cpp","components/football/request_scheduler.cpp","components/football/budget_ledger.cpp","components/football/store_views.cpp","components/football/screen_selection.cpp","components/ui/navigation_policy.cpp","components/ui/screen_sequence.cpp","components/ui/football_presenter.cpp","components/ui/status_presenter.cpp","components/ui/cause_text.cpp","components/settings/settings_model.cpp","components/settings/screen_policy.cpp","components/settings/ui_config.cpp","components/timekeeping/civil_time.cpp","components/network/wifi_policy.cpp","scripts/tests/fixtures/ui_presentation_host.cpp"]:
        command.append(str(root/source))
    binary=scratch/"presentation"
    subprocess.run(command+["-o",str(binary)],check=True,cwd=root)
    subprocess.run([str(binary)],check=True,cwd=root)
