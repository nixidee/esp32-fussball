"""Embed the offline Web UI as deterministic gzip, without runtime dependencies."""
from pathlib import Path
import gzip
import sys

source, destination = map(Path, sys.argv[1:])
html = "\n".join(line.strip() for line in source.read_text().splitlines()).encode()
payload = gzip.compress(html, compresslevel=9, mtime=0)
destination.write_text("#pragma once\n#include <cstddef>\n"
                       "inline constexpr unsigned char kWebAsset[] = {\n" +
                       ",".join(str(byte) for byte in payload) +
                       "\n};\ninline constexpr std::size_t kWebAssetBytes = sizeof(kWebAsset);\n")
