"""Check the formatting of changed C/C++ lines (check only, never rewrites).

Run with the Python of the project's tool environment, see README.md:
    .venv-tools/bin/python scripts/check_format.py [<base commit>]

Lines changed against <base commit> (default HEAD, i.e. uncommitted changes)
must match .clang-format; unchanged code is not checked. New files are seen
only once Git knows them (git add, or git add -N). Exit code 0 = formatted,
1 = deviations (printed as a diff), 2 = tool setup problem.
"""

import re
import subprocess
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
# Own sources only; managed_components/ and .pio/ are outside these folders.
SOURCE_DIRS = ("boards", "components", "displays", "include", "src",
               "targets", "test")


def pinned_version():
    text = (PROJECT_DIR / "requirements-tools.txt").read_text()
    match = re.search(r"^clang-format==(\S+)$", text, re.MULTILINE)
    return match.group(1) if match else None


def main():
    tools = Path(sys.executable).parent
    clang_format = tools / "clang-format"
    git_clang_format = tools / "git-clang-format"
    if sys.platform == "win32":
        clang_format = clang_format.with_suffix(".exe")
        git_clang_format = git_clang_format.with_suffix(".exe")
    if not clang_format.is_file() or not git_clang_format.is_file():
        print("clang-format not found next to %s; run this script with the "
              "tool environment's Python (README.md)." % sys.executable)
        return 2

    version = subprocess.run([str(clang_format), "--version"],
                             capture_output=True, text=True).stdout
    wanted = pinned_version()
    if wanted is None or ("version %s" % wanted) not in version:
        print("clang-format %s required (requirements-tools.txt), found: %s"
              % (wanted, version.strip()))
        return 2

    base = sys.argv[1] if len(sys.argv) > 1 else "HEAD"
    result = subprocess.run(
        [str(git_clang_format), "--binary", str(clang_format), "--diff",
         base, "--", *SOURCE_DIRS],
        cwd=PROJECT_DIR, capture_output=True, text=True)
    # git-clang-format --diff: 0 = nothing to change, 1 = diff printed.
    if result.returncode == 0:
        print("Formatting OK (changed lines since %s)." % base)
        return 0
    print((result.stdout + result.stderr).strip())
    return 1 if result.stdout.startswith("diff") else 2


if __name__ == "__main__":
    sys.exit(main())
