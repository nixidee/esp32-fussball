"""Publish generated build bytes without rebuilding unchanged consumers."""

from pathlib import Path


def write_if_changed(output: Path, content: bytes) -> bool:
    """Replace through a sibling temporary file; return whether bytes changed.

    Identical bytes preserve the output timestamp. A failed write or replace
    leaves the previous output intact and removes the temporary file.
    """
    output = Path(output)
    if output.is_file() and output.read_bytes() == content:
        return False
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    try:
        temporary.write_bytes(content)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return True
