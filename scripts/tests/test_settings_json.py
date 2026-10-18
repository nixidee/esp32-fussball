"""Exercise production typed settings codecs with ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="football-settings-json-") as folder:
    binary = Path(folder) / "settings_json"
    command = ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for include in ["include", "components/json/include", "components/settings/include",
                    "components/football/include", "components/timekeeping/include", "components/web"]:
        command += ["-I", str(root / include)]
    for source in ["components/json/json_stream.cpp", "components/settings/settings_model.cpp",
                   "components/settings/screen_policy.cpp", "components/timekeeping/civil_time.cpp",
                   "components/football/football_model.cpp", "components/web/settings_json.cpp", "components/web/screen_json.cpp",
                   "scripts/tests/settings_json_host.cpp"]:
        command.append(str(root / source))
    subprocess.run(command + ["-o", str(binary)], check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
