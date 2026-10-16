"""Check the JPEG contract and refresh stale project-owned CMake inputs.

PlatformIO's ESP-IDF builder does not watch component CMakeLists.txt files,
included CMake helpers or VERSION. Run the existing source contract even
when CMake is cached, then invalidate only build.ninja when those inputs
change. SDK configuration and certificate-bundle freshness have a separate
owner in sdkconfig_guard.py.
"""

import hashlib
import os
from pathlib import Path
import subprocess

FINGERPRINT_FILE = "cmake_inputs.sha256"
UNCHECKED_TARGETS = {"menuconfig", "clean", "fullclean", "erase", "monitor"}


class GuardError(RuntimeError):
    """An unavailable tool or rejected source contract stops the build."""


def cmake_inputs(project_dir, source_dir):
    """Include file names as well as contents to notice additions/removals."""
    return sorted({
        project_dir / "CMakeLists.txt",
        source_dir / "CMakeLists.txt",
        project_dir / "VERSION",
        *project_dir.glob("components/**/CMakeLists.txt"),
        *project_dir.glob("scripts/*.cmake"),
    })


def fingerprint(project_dir, source_dir):
    digest = hashlib.sha256(b"project-cmake-inputs-v1\0")
    for path in cmake_inputs(project_dir, source_dir):
        name = (path.relative_to(project_dir)
                if path.is_relative_to(project_dir) else path).as_posix()
        present = path.is_file()
        contents = path.read_bytes() if present else b""
        digest.update(name.encode("utf-8") + b"\0")
        digest.update(b"file\0" if present else b"missing\0")
        digest.update(str(len(contents)).encode("ascii") + b"\0" + contents)
    return digest.hexdigest()


def check_sources(cmake, project_dir):
    """CMake owns all reviewed JPEG source hashes; do not duplicate them."""
    result = subprocess.run(
        [str(cmake), "-P",
         str(project_dir / "scripts/jpeg_observer_guard.cmake")],
        cwd=project_dir, capture_output=True, text=True,
    )
    if result.returncode:
        raise GuardError((result.stdout + result.stderr).strip())


def run_platformio(env, targets):
    # PlatformIO translates clean/fullclean to SCons --clean, leaving the
    # named target list empty. A subsequent firmware pass checks normally.
    if env.IsCleanTarget():
        return
    targets = set(targets)
    if targets and targets <= UNCHECKED_TARGETS:
        return
    try:
        package = env.PioPlatform().get_package_dir("tool-cmake")
        if not package:
            raise GuardError("installed tool-cmake package not found")
        cmake = Path(package) / "bin" / (
            "cmake.exe" if os.name == "nt" else "cmake")
        if not cmake.is_file():
            raise GuardError("installed CMake executable not found: %s" % cmake)
        project_dir = Path(env.subst("$PROJECT_DIR"))
        source_dir = Path(env.subst("$PROJECT_SRC_DIR"))
        build_dir = Path(env.subst("$BUILD_DIR"))
        lvgl_dir = project_dir / "managed_components/lvgl__lvgl"
        bootstrap = not lvgl_dir.exists() and not lvgl_dir.is_symlink()
        # A fresh checkout resolves managed components during CMake. Its
        # existing component guard checks them before generating the graph.
        # An incomplete existing installation must still fail immediately.
        if not bootstrap:
            check_sources(cmake, project_dir)
        current = fingerprint(project_dir, source_dir)
        stamp = build_dir / FINGERPRINT_FILE
        recorded = stamp.read_text().strip() if stamp.is_file() else ""
        if bootstrap or recorded != current:
            (build_dir / "build.ninja").unlink(missing_ok=True)
            build_dir.mkdir(parents=True, exist_ok=True)
            stamp.write_text(current + "\n")
            # A failed subsequent configuration still leaves build.ninja
            # absent, so PlatformIO retries it even with this stamp present.
            reason = ("LVGL bootstrap requires configuration" if bootstrap
                      else "project inputs changed")
            print("CMake guard: %s; refreshing build graph" % reason)
    except (GuardError, OSError, UnicodeError) as error:
        print("Error (CMake guard): %s" % error)
        env.Exit(1)


if "Import" in globals():
    from SCons.Script import COMMAND_LINE_TARGETS

    Import("env")
    run_platformio(env, COMMAND_LINE_TARGETS)
