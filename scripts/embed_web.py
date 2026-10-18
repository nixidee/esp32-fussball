"""Embed the offline Web UI as deterministic gzip, without runtime dependencies.

PlatformIO runs this pre-script on every build, including cached CMake runs.
A changed HTML, stylesheet or script does not make PlatformIO reconfigure
CMake, so CMake cannot generate the header (W129). Local source files are
inlined in the order declared by web/index.html into one offline document.
The header is rewritten only when its bytes change, so unchanged sources do
not recompile the web component. No JavaScript transform or runtime loader
is involved.
"""

import argparse
import gzip
import re
from pathlib import Path

SOURCE_RELATIVE_PATH = Path("web") / "index.html"
OUTPUT_RELATIVE_PATH = Path("web_asset") / "web_asset.h"
STYLESHEET = re.compile(r'<link\s+rel="stylesheet"\s+href="([^"]+)"\s*/?>')
SCRIPT = re.compile(r'<script\s+src="([^"]+)"\s*>\s*</script>')


def render_document(source):
    """Inline local styles/scripts into one document with one script scope."""
    source = Path(source).resolve()
    root = source.parent
    html = source.read_text()

    def local_text(reference):
        path = (root / reference).resolve()
        if not path.is_relative_to(root):
            raise ValueError("Web source escapes its directory: " + reference)
        return path.read_text()

    html = STYLESHEET.sub(
        lambda match: "<style>\n" + local_text(match[1]) + "</style>", html
    )
    scripts = list(SCRIPT.finditer(html))
    if scripts:
        content = "\n".join(local_text(match[1]).rstrip() for match in scripts)
        # A single scope preserves the original application's lexical state.
        # HTML source order is the only declaration order used by any loader.
        first = True

        def script_tag(_match):
            nonlocal first
            if not first:
                return ""
            first = False
            return "<script>\n" + content + "\n</script>"

        html = SCRIPT.sub(script_tag, html)
    return html


def render_header(source):
    """Returns the header text and the gzip payload size."""
    html = "\n".join(
        line.strip() for line in render_document(source).splitlines()
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
    except (OSError, UnicodeError, ValueError) as error:
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
    parser.add_argument("build_dir", type=Path, nargs="?")
    parser.add_argument("--document", action="store_true",
                        help="Print the bundled HTML without writing files")
    args = parser.parse_args()
    try:
        if args.document:
            print(render_document(args.source), end="")
            raise SystemExit(0)
        if args.build_dir is None:
            parser.error("build_dir is required unless --document is used")
        output, changed, size = prepare_header(args.source, args.build_dir)
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, "Error (web asset): %s\n" % error)
    print("%s: %s (%d bytes gzip)"
          % ("generated" if changed else "unchanged", output, size))
