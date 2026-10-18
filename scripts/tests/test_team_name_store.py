#!/usr/bin/env python3
"""Compile and exercise the production bounded codec against the full source."""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sanitize", action="store_true",
                    help="enable AddressSanitizer and UndefinedBehaviorSanitizer")
args = parser.parse_args()
ROOT = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="team-name-store-") as tmp:
    build = pathlib.Path(tmp)
    subprocess.run(["python3", str(ROOT / "scripts/embed_team_names.py"),
                    str(ROOT / "include/team_names.json"), str(build)], check=True)
    command = ["c++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
               "-DFUSSBALL_NATIVE", "-I" + str(ROOT / "include"),
               "-I" + str(ROOT / "components/names/include"),
               "-I" + str(build / "team_name_asset"),
               str(ROOT / "components/names/team_name_store.cpp"),
               str(ROOT / "components/names/team_name_catalogue.cpp"),
            str(ROOT / "components/json/json_stream.cpp"),
            "-I" + str(ROOT / "components/json/include"),
               str(ROOT / "scripts/tests/fixtures/team_name_store_test.cpp"),
               "-o", str(build / "test")]
    if args.sanitize:
        command[command.index("-O2")] = "-O1"
        command[1:1] = ["-g", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True)
    subprocess.run([str(build / "test")], check=True)
