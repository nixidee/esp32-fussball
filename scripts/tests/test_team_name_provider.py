"""Check production Reader/facts/mapper names with host sanitizers."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    includes = [ROOT / "include", ROOT / ".pio/libdeps/native/ArduinoJson/src"]
    includes.extend((ROOT / "components").glob("*/include"))
    sources = [ROOT / "scripts/tests/team_name_provider_host.cpp"]
    sources.extend(ROOT / "components/football" / name for name in (
        "provider_readers.cpp", "provider_adapter.cpp", "store_mappers.cpp", "provider_plan.cpp", "fixture_retention.cpp", "football_model.cpp", "match_store.cpp",
        "matchday.cpp", "screen_selection.cpp", "store_views.cpp", "request_scheduler.cpp",
        "budget_ledger.cpp"))
    sources += [ROOT / "components/settings/settings_model.cpp",
                ROOT / "components/settings/screen_policy.cpp",
                ROOT / "components/json/json_stream.cpp",
                ROOT / "components/timekeeping/civil_time.cpp"]
    with tempfile.TemporaryDirectory(prefix="team-name-provider-") as temporary:
        executable = Path(temporary) / "provider-check"
        command = [shutil.which("clang++") or "clang++", "-std=c++20", "-O1",
                   "-Wall", "-Wextra", "-Werror", "-fno-omit-frame-pointer",
                   "-fsanitize=address,undefined"]
        command.extend("-I" + str(path) for path in includes)
        command.extend(map(str, sources))
        command += ["-o", str(executable)]
        subprocess.run(command, check=True)
        environment = dict(os.environ)
        leaks = 0 if sys.platform == "darwin" else 1
        environment["ASAN_OPTIONS"] = f"detect_leaks={leaks}:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        subprocess.run([str(executable)], check=True, env=environment)


if __name__ == "__main__":
    main()
