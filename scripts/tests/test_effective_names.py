"""ASan/UBSan check of actual bounded effective-name and provenance code."""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    compiler = shutil.which("clang++")
    if compiler is None:
        raise SystemExit("clang++ is required for projection sanitizer checks")
    with tempfile.TemporaryDirectory(prefix="team-name-projection-") as temporary:
        binary = Path(temporary) / "check"
        subprocess.run([
            compiler, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I" + str(root / "include"),
            "-I" + str(root / "components/settings/include"),
            "-I" + str(root / "components/football/include"),
            "-I" + str(root / "components/names/include"),
            str(root / "scripts/tests/effective_names_host.cpp"),
            str(root / "components/names/team_name_store.cpp"),
            str(root / "components/names/effective_names.cpp"),
            "-o", str(binary),
        ], cwd=root, check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = (
            "detect_leaks=" + ("0" if sys.platform == "darwin" else "1")
            + ":abort_on_error=1"
        )
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        subprocess.run([str(binary)], cwd=root, env=environment, check=True)


if __name__ == "__main__":
    main()
