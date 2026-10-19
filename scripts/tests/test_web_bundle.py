"""Offline web source ordering and incremental asset publication."""

import gzip
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("web_bundle", ROOT / "scripts/embed_web.py")
BUNDLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUNDLE)


class WebBundleTest(unittest.TestCase):
    def test_actual_platformio_entry_does_not_require_file_global(self):
        with tempfile.TemporaryDirectory() as build_dir:
            class Environment:
                def subst(self, key):
                    return {"$PROJECT_DIR": str(ROOT), "$BUILD_DIR": build_dir}[key]

                def Exit(self, code):
                    raise AssertionError("PlatformIO generator failed: " + str(code))

            scope = {"Import": lambda name: None, "env": Environment()}
            script = ROOT / "scripts/embed_web.py"
            exec(compile(script.read_text(), str(script), "exec"), scope)
            self.assertTrue((Path(build_dir) / BUNDLE.OUTPUT_RELATIVE_PATH).is_file())

    def test_bootstrap_policy_matches_the_actual_firmware_schema(self):
        fixture = json.loads(subprocess.check_output(
            ["python3", str(ROOT / "scripts/tests/web_config_fixture.py")],
            text=True))
        html = BUNDLE.render_document(ROOT / "web/index.html")
        client = json.loads(re.search(
            r"const CLIENT = Object.freeze\((\{[^\n]+\})\);", html)[1])
        self.assertEqual(client, fixture["schema"]["client"])

    def test_real_document_is_one_offline_script_and_stylesheet(self):
        html = BUNDLE.render_document(ROOT / "web/index.html")
        self.assertEqual(html.count("<script>"), 1)
        self.assertEqual(html.count("<style>"), 1)
        self.assertNotRegex(html, r'<script\s+src=|<link\s+rel="stylesheet"')
        subprocess.run(["node", "--check"], input=re.search(
            r"<script>([\s\S]*?)</script>", html)[1], text=True, check=True)

    def test_declared_order_shares_one_lexical_scope(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "index.html").write_text(
                '<script src="first.js"></script><script src="second.js"></script>')
            (root / "first.js").write_text("const selected = 42;\n")
            (root / "second.js").write_text("if (selected !== 42) throw Error('wrong order');\n")
            html = BUNDLE.render_document(root / "index.html")
            script = re.search(r"<script>([\s\S]*?)</script>", html)[1]
            subprocess.run(["node", "-"], input=script, text=True, check=True)

    def test_module_edits_replace_asset_and_unchanged_sources_keep_it(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "index.html"
            source.write_text('<script src="app.js"></script>')
            script = root / "app.js"
            script.write_text("const value = 1;\n")
            output, changed, _ = BUNDLE.prepare_header(source, root / "build")
            self.assertTrue(changed)
            before = output.read_bytes()
            timestamp = output.stat().st_mtime_ns
            self.assertFalse(BUNDLE.prepare_header(source, root / "build")[1])
            self.assertEqual(output.stat().st_mtime_ns, timestamp)
            script.write_text("const value = 2;\n")
            self.assertTrue(BUNDLE.prepare_header(source, root / "build")[1])
            self.assertNotEqual(output.read_bytes(), before)

    def test_header_payload_is_deterministic_bundled_html(self):
        source = ROOT / "web/index.html"
        header, size = BUNDLE.render_header(source)
        self.assertEqual((header, size), BUNDLE.render_header(source))
        encoded = re.search(r"kWebAsset\[\] = \{\s*([\d,]+)", header)[1]
        payload = bytes(map(int, encoded.split(",")))
        self.assertEqual(len(payload), size)
        expected = "\n".join(line.strip() for line in BUNDLE.render_document(source).splitlines()).encode()
        self.assertEqual(gzip.decompress(payload), expected)

    def test_local_source_cannot_escape_web_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "web").mkdir()
            source = root / "web/index.html"
            source.write_text('<script src="../outside.js"></script>')
            (root / "outside.js").write_text("const outside = true;")
            with self.assertRaises(ValueError):
                BUNDLE.render_document(source)


if __name__ == "__main__":
    unittest.main()
