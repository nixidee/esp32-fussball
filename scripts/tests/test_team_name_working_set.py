"""Exercise actual compact provider directory and overflow admission."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    includes = [ROOT / "include"]
    includes.extend((ROOT / "components").glob("*/include"))
    sources = [ROOT / "scripts/tests/team_name_working_set_host.cpp"]
    sources.extend(ROOT / "components/football" / name for name in (
        "match_store.cpp", "store_mappers.cpp", "football_model.cpp",
        "matchday.cpp", "store_views.cpp"))
    sources.append(ROOT / "components/timekeeping/civil_time.cpp")
    with tempfile.TemporaryDirectory(prefix="team-name-working-set-") as temporary:
        executable = Path(temporary) / "working-set-check"
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
