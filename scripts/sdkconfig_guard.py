"""Keep the generated sdkconfig.<env> in step with its inputs.

Runs before the ESP-IDF builder (pre: script). ESP-IDF fills only unset
options from SDKCONFIG_DEFAULTS, and PlatformIO reconfigures only for a few
watched files, so an edited defaults file would otherwise leave a stale
sdkconfig.<env> in use. A fingerprint over every configuration input is kept
in the build folder; when it differs, sdkconfig.<env> is deleted and ESP-IDF
generates it again from the defaults files.

The verification after configuration lives in sdkconfig_verify.py (post:
script) and calls verify_sdkconfig() registered here: every option in the
defaults files must appear with the same value in sdkconfig.<env>, and the
partition CSV must match CONFIG_PARTITION_TABLE_CUSTOM_FILENAME.
"""

import hashlib
import shlex
from pathlib import Path

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")

FINGERPRINT_FILE = "sdkconfig_inputs.sha256"
DEPRECATED_MARKER = "# Deprecated options for backward compatibility"
# Runs consisting only of these targets skip the verification (menuconfig must
# stay usable to inspect a configuration that fails it).
UNCHECKED_TARGETS = {"menuconfig", "clean", "fullclean", "erase", "monitor"}

board = env.BoardConfig()
project_dir = Path(env.subst("$PROJECT_DIR"))
build_dir = Path(env.subst("$BUILD_DIR"))
sdkconfig_path = Path(
    board.get(
        "build.esp-idf.sdkconfig_path",
        str(project_dir / ("sdkconfig.%s" % env.subst("$PIOENV"))),
    )
)


def fail(message):
    print("Error (sdkconfig guard): " + message)
    env.Exit(1)


def defaults_files():
    """Files of SDKCONFIG_DEFAULTS in the env's cmake_extra_args."""
    for arg in shlex.split(board.get("build.cmake_extra_args", "")):
        if arg.startswith("-DSDKCONFIG_DEFAULTS="):
            names = arg.split("=", 1)[1].split(";")
            return [project_dir / name for name in names if name]
    fail("-DSDKCONFIG_DEFAULTS is missing in board_build.cmake_extra_args")
    return []


def partition_csv():
    name = board.get("build.partitions", "")
    if not name:
        fail("board_build.partitions is not set")
    return name


def config_inputs():
    """Every file whose change must regenerate sdkconfig.<env>."""
    framework_dir = Path(env.PioPlatform().get_package_dir("framework-espidf"))
    inputs = defaults_files() + [project_dir / partition_csv()]
    inputs += sorted(project_dir.glob("components/*/idf_component.yml"))
    inputs += [
        Path(env.subst("$PROJECT_SRC_DIR")) / "idf_component.yml",
        project_dir / ("dependencies.lock.%s" % board.get("build.mcu")),
        framework_dir / "version.txt",
    ]
    return inputs


def fingerprint():
    digest = hashlib.sha256()
    digest.update(env.PioPlatform().version.encode())
    for path in config_inputs():
        digest.update(str(path.relative_to(project_dir)
                          if path.is_relative_to(project_dir) else path)
                      .encode())
        digest.update(path.read_bytes() if path.is_file() else b"<missing>")
    return digest.hexdigest()


def parse_options(lines):
    """Maps option name to value; "n" for '# CONFIG_X is not set'."""
    options = {}
    for line in lines:
        line = line.strip()
        if line.startswith("CONFIG_") and "=" in line:
            name, value = line.split("=", 1)
            options[name] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            options[line[2:-len(" is not set")]] = "n"
    return options


def verify_sdkconfig(env):
    targets = set(COMMAND_LINE_TARGETS)
    if targets and targets <= UNCHECKED_TARGETS:
        return
    generated = sdkconfig_path.read_text().splitlines()
    split = (generated.index(DEPRECATED_MARKER)
             if DEPRECATED_MARKER in generated else len(generated))
    current = parse_options(generated[:split])
    deprecated = parse_options(generated[split:])

    errors = []
    for path in defaults_files():
        if not path.is_file():
            errors.append("%s: file not found" % path.name)
            continue
        for name, wanted in parse_options(path.read_text().splitlines()).items():
            if name in current:
                if current[name] != wanted:
                    errors.append("%s: %s=%s, generated %s"
                                  % (path.name, name, wanted, current[name]))
            elif name in deprecated:
                errors.append("%s: %s is a deprecated name; use the current "
                              "option name" % (path.name, name))
            else:
                errors.append("%s: %s is unknown or its dependencies are not "
                              "met" % (path.name, name))

    table = current.get("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME", "").strip('"')
    if table != partition_csv():
        errors.append("board_build.partitions=%s, "
                      "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=%s"
                      % (partition_csv(), table or "<unset>"))

    stamp = build_dir / FINGERPRINT_FILE
    if errors:
        # Without a recorded fingerprint the next build regenerates the file.
        stamp.unlink(missing_ok=True)
        fail("%s does not match its defaults files or partition CSV:\n  "
             "%s\nFix the defaults files or platformio.ini (settings made in "
             "menuconfig belong in a defaults file); the next build "
             "regenerates %s."
             % (sdkconfig_path.name, "\n  ".join(errors), sdkconfig_path.name))
    # Recorded only for a verified configuration, after ESP-IDF may have
    # updated the lockfile during configuration.
    build_dir.mkdir(parents=True, exist_ok=True)
    stamp.write_text(fingerprint() + "\n")


env.AddMethod(verify_sdkconfig, "VerifySdkconfig")

stamp = build_dir / FINGERPRINT_FILE
recorded = stamp.read_text().strip() if stamp.is_file() else ""
if recorded != fingerprint() and sdkconfig_path.is_file():
    print("sdkconfig guard: configuration inputs changed, regenerating %s"
          % sdkconfig_path.name)
    sdkconfig_path.unlink()
