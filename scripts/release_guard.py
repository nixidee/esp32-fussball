"""Reject release builds with a development version or device test selector."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
version = (root / "VERSION").read_text().strip()
if not re.fullmatch(r"\d+\.\d+\.\d+", version):
    raise SystemExit("Release requires a plain MAJOR.MINOR.PATCH version in VERSION")
config = (root / "include/app_config.h").read_text()
for name, kind in [("DisplayFault", "DisplayFault"), ("EventTest", "EventTest"),
                   ("FileTest", "FileTest"), ("TimeTest", "TimeTest"),
                   ("HealthTest", "HealthTest"), ("SettingsTest", "SettingsTest")]:
    if not re.search(rf"k{name}\s*=\s*{kind}::kNone\s*;", config):
        raise SystemExit(f"Release requires k{name} = {kind}::kNone")
print(f"Release source guard: {version}, all six test selectors disabled")
