"""Build publication preserves prior bytes/timestamps and handles I/O faults."""

from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from generated_file import write_if_changed


class GeneratedFileTest(unittest.TestCase):
    def test_creation_change_and_unchanged_timestamp(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "nested/asset.h"
            self.assertTrue(write_if_changed(output, b"first"))
            timestamp = output.stat().st_mtime_ns
            with mock.patch.object(Path, "write_bytes") as write:
                self.assertFalse(write_if_changed(output, b"first"))
                write.assert_not_called()
            self.assertEqual(output.stat().st_mtime_ns, timestamp)
            self.assertTrue(write_if_changed(output, b"second"))
            self.assertEqual(output.read_bytes(), b"second")
            self.assertEqual(list(output.parent.iterdir()), [output])

    def test_partial_write_failure_preserves_previous_output(self):
        write = Path.write_bytes

        def partial(path, content):
            write(path, content[:2])
            raise OSError("injected short write")

        for suffix in (".h", ".c"):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / ("asset" + suffix)
                output.write_bytes(b"previous")
                timestamp = output.stat().st_mtime_ns
                with mock.patch.object(Path, "write_bytes", partial):
                    with self.assertRaisesRegex(OSError, "injected short write"):
                        write_if_changed(output, b"replacement")
                self.assertEqual(output.read_bytes(), b"previous")
                self.assertEqual(output.stat().st_mtime_ns, timestamp)
                self.assertEqual(list(output.parent.iterdir()), [output])

    def test_replace_failure_preserves_output_and_next_run_recovers(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "asset.h"
            output.write_bytes(b"previous")
            with mock.patch.object(Path, "replace", side_effect=OSError("replace failed")):
                with self.assertRaisesRegex(OSError, "replace failed"):
                    write_if_changed(output, b"replacement")
            self.assertEqual(output.read_bytes(), b"previous")
            self.assertEqual(list(output.parent.iterdir()), [output])
            self.assertTrue(write_if_changed(output, b"replacement"))
            self.assertEqual(output.read_bytes(), b"replacement")


if __name__ == "__main__":
    unittest.main()
