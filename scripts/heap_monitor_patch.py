"""Generate the approved, project-local ESP-IDF heap-monitor correction.

PlatformIO runs this pre-script every time, including cached CMake runs.
The shared SDK is only read. CMake substitutes the generated source into
its existing heap target; normal PlatformIO middleware bypasses IDF sources.
"""

import argparse
import hashlib
import re
from pathlib import Path

EXPECTED_IDF_VERSION = "6.1.0"
EXPECTED_SOURCE_SHA256 = (
    "6576a54104902164e726a0a6149e01cbbbe1aa1c5a388125bc77cb587ff911b4"
)
OUTPUT_RELATIVE_PATH = Path("heap_monitor_patch") / "heap_caps.c"
INCLUDE_ANCHOR = '#include "esp_system.h"\n'
ALLOCATION_ANCHOR = """        min_free_bytes_monitoring.values = heap_caps_malloc(sizeof(size_t) * min_free_bytes_monitoring.counter, MALLOC_CAP_DEFAULT);
        assert(min_free_bytes_monitoring.values != NULL && "not enough memory to store min_free_bytes value");
"""
HOOK_DECLARATION = """
#ifdef CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS
#error "Heap-monitor error recovery requires allocation aborts to be disabled"
#endif

// The device test may provide this hook. It runs under the monitor lock:
// only consume a flag here; never allocate, log, block, or call heap APIs.
extern bool fussball_heap_monitor_fail_allocation(void) __attribute__((weak));
"""
ALLOCATION_REPLACEMENT = """        bool fail_allocation = fussball_heap_monitor_fail_allocation != NULL &&
                               fussball_heap_monitor_fail_allocation();
        min_free_bytes_monitoring.values = fail_allocation ? NULL :
            heap_caps_malloc(sizeof(size_t) * min_free_bytes_monitoring.counter, MALLOC_CAP_DEFAULT);
        if (min_free_bytes_monitoring.values == NULL) {
            min_free_bytes_monitoring.counter = 0;
            MULTI_HEAP_UNLOCK(&min_free_bytes_monitoring.mux);
            return ESP_ERR_NO_MEM;
        }
"""


class PatchError(RuntimeError):
    """An unsupported SDK or source layout must stop the build."""


def read_idf_version(framework_dir):
    text = (framework_dir / "tools/cmake/version.cmake").read_text()
    parts = []
    for name in ("MAJOR", "MINOR", "PATCH"):
        matches = re.findall(
            r"^set\(IDF_VERSION_" + name + r"\s+(\d+)\)\s*$",
            text,
            re.MULTILINE,
        )
        if len(matches) != 1:
            raise PatchError("cannot identify the exact ESP-IDF version")
        parts.append(matches[0])
    return ".".join(parts)


def patch_source(source):
    for name, anchor in (
        ("include", INCLUDE_ANCHOR),
        ("monitor allocation", ALLOCATION_ANCHOR),
    ):
        if source.count(anchor) != 1:
            raise PatchError("expected exactly one %s anchor" % name)
    return source.replace(
        INCLUDE_ANCHOR, INCLUDE_ANCHOR + HOOK_DECLARATION, 1
    ).replace(ALLOCATION_ANCHOR, ALLOCATION_REPLACEMENT, 1)


def prepare_patch(framework_dir, build_dir):
    """Validate on every run; write the local copy only when bytes differ."""
    framework_dir = Path(framework_dir)
    build_dir = Path(build_dir)
    version = read_idf_version(framework_dir)
    if version != EXPECTED_IDF_VERSION:
        raise PatchError("expected ESP-IDF %s, found %s"
                         % (EXPECTED_IDF_VERSION, version))
    original = framework_dir / "components/heap/heap_caps.c"
    source = original.read_bytes()
    digest = hashlib.sha256(source).hexdigest()
    if digest != EXPECTED_SOURCE_SHA256:
        raise PatchError("unsupported heap_caps.c SHA256 %s; expected %s"
                         % (digest, EXPECTED_SOURCE_SHA256))
    patched = patch_source(source.decode("utf-8")).encode("utf-8")
    output = build_dir / OUTPUT_RELATIVE_PATH
    if output.is_file() and output.read_bytes() == patched:
        return output, False
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".c.tmp")
    temporary.write_bytes(patched)
    temporary.replace(output)
    return output, True


def run_platformio(env):
    framework_dir = env.PioPlatform().get_package_dir("framework-espidf")
    if not framework_dir:
        print("Error (heap monitor patch): ESP-IDF package not found")
        env.Exit(1)
        return
    try:
        output, changed = prepare_patch(framework_dir, env.subst("$BUILD_DIR"))
    except (PatchError, OSError, UnicodeError) as error:
        print("Error (heap monitor patch): %s" % error)
        env.Exit(1)
        return
    if changed:
        print("heap monitor patch: generated %s" % output)


if "Import" in globals():
    Import("env")
    run_platformio(env)
elif __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("framework_dir", type=Path)
    parser.add_argument("build_dir", type=Path)
    args = parser.parse_args()
    try:
        output, changed = prepare_patch(args.framework_dir, args.build_dir)
    except (PatchError, OSError, UnicodeError) as error:
        parser.exit(1, "Error (heap monitor patch): %s\n" % error)
    print("%s: %s" % ("generated" if changed else "unchanged", output))
