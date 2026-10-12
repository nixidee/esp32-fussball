"""Embed the offline Web UI as deterministic gzip, without runtime dependencies.

PlatformIO runs this pre-script on every build, including cached CMake runs.
A changed web/index.html does not make PlatformIO reconfigure CMake, so CMake
cannot generate the header (W129); components/web/CMakeLists.txt only adds the
output directory to the include path. The header is rewritten only when its
bytes change, so an unchanged page does not recompile the web component.
"""

import argparse
import gzip
from pathlib import Path

SOURCE_RELATIVE_PATH = Path("web") / "index.html"
OUTPUT_RELATIVE_PATH = Path("web_asset") / "web_asset.h"


def render_header(source):
    """Returns the header text and the gzip payload size."""
    html = "\n".join(
        line.strip() for line in Path(source).read_text().splitlines()
    ).encode()
    payload = gzip.compress(html, compresslevel=9, mtime=0)
    text = (
        "#pragma once\n#include <cstddef>\n"
        "inline constexpr unsigned char kWebAsset[] = {\n"
        + ",".join(str(byte) for byte in payload)
        + "\n};\ninline constexpr std::size_t kWebAssetBytes = sizeof(kWebAsset);\n"
    )
    return text, len(payload)


def prepare_header(source, build_dir):
    """Returns (output, changed, gzip bytes); writes only when bytes differ."""
    text, size = render_header(source)
    content = text.encode("utf-8")
    output = Path(build_dir) / OUTPUT_RELATIVE_PATH
    if output.is_file() and output.read_bytes() == content:
        return output, False, size
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".h.tmp")
    temporary.write_bytes(content)
    temporary.replace(output)
    return output, True, size


def run_platformio(env):
    source = Path(env.subst("$PROJECT_DIR")) / SOURCE_RELATIVE_PATH
    try:
        output, changed, size = prepare_header(source, env.subst("$BUILD_DIR"))
    except (OSError, UnicodeError) as error:
        print("Error (web asset): %s" % error)
        env.Exit(1)
        return
    if changed:
        print("web asset: generated %s (%d bytes gzip)" % (output, size))


if "Import" in globals():
    Import("env")
    run_platformio(env)
elif __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("build_dir", type=Path)
    args = parser.parse_args()
    try:
        output, changed, size = prepare_header(args.source, args.build_dir)
    except (OSError, UnicodeError) as error:
        parser.exit(1, "Error (web asset): %s\n" % error)
    print("%s: %s (%d bytes gzip)"
          % ("generated" if changed else "unchanged", output, size))
