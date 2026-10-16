"""Exercise the real pre-hook and CMake against isolated build artifacts.

Only the SCons environment and dependency-resolution boundary are adapted.
The JPEG contract, its managed source files and PlatformIO's cache
decision function are read from the installed/current production sources.
"""

import ast
import configparser
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
HOOK = ROOT / "scripts/cmake_guard.py"
GUARD = ROOT / "scripts/jpeg_observer_guard.cmake"
PIO = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
CMAKE_PACKAGE = PIO / "packages/tool-cmake"
CMAKE = CMAKE_PACKAGE / "bin" / ("cmake.exe" if os.name == "nt" else "cmake")
NINJA = PIO / "packages/tool-ninja" / ("ninja.exe" if os.name == "nt" else "ninja")
ESP_IDF_BUILDER = PIO / "platforms/espressif32/builder/frameworks/espidf.py"


class BoundaryEnvironment:
    def __init__(self, project, build, cmake_package=CMAKE_PACKAGE):
        self.project = project
        self.source = project / "src"
        self.build = build
        self.cmake_package = cmake_package
        self.package_reads = 0
        self.clean = False

    def IsCleanTarget(self):
        return self.clean

    def subst(self, name):
        return str({"$PROJECT_DIR": self.project,
                    "$PROJECT_SRC_DIR": self.source,
                    "$BUILD_DIR": self.build}[name])

    def PioPlatform(self):
        return self

    def get_package_dir(self, name):
        if name != "tool-cmake":
            raise AssertionError("Unexpected tool dependency: " + name)
        self.package_reads += 1
        return str(self.cmake_package) if self.cmake_package else None

    def Exit(self, code):
        raise SystemExit(code)


class CmakeGuardTests(unittest.TestCase):
    def setUp(self):
        for path in (CMAKE, NINJA, ESP_IDF_BUILDER):
            if not path.is_file():
                self.fail("Required installed test input missing: " + str(path))
        self.temporary = tempfile.TemporaryDirectory(prefix="cmake-guard-")
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name) / "project"
        self.build = self.project / "build"
        self.project.mkdir()
        self.build.mkdir()
        for name in ("scripts", "src", "components/display", "framework"):
            (self.project / name).mkdir(parents=True)
        shutil.copyfile(GUARD, self.project / "scripts/jpeg_observer_guard.cmake")
        # Read path names from the only contract; do not duplicate its hashes.
        self.contracts = re.findall(r'"([^"=]+)=[a-f0-9]{64}"', GUARD.read_text())
        self.assertGreater(len(self.contracts), 0)
        self.lvgl = self.project / "managed_components/lvgl__lvgl"
        for relative in self.contracts:
            target = self.lvgl / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / "managed_components/lvgl__lvgl" / relative, target)
        self.cmake_file = self.project / "components/display/CMakeLists.txt"
        self.cmake_file.write_text(
            'add_custom_target(display_fixture DEPENDS display_port.cpp)\n')
        for name in ("display_port.cpp", "jpeg_observer.cpp"):
            (self.cmake_file.parent / name).write_text("// Test build-graph input.\n")
        (self.project / "src/CMakeLists.txt").write_text("# Watched source input.\n")
        (self.project / "VERSION").write_text("0.1.0-dev\n")
        (self.project / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.22)\n'
            'project(cmake_guard_fixture NONE)\n'
            'if(NOT EXISTS "${CMAKE_SOURCE_DIR}/managed_components/lvgl__lvgl")\n'
            '  file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/managed_components")\n'
            '  file(COPY "${CMAKE_SOURCE_DIR}/resolved/lvgl__lvgl"\n'
            '       DESTINATION "${CMAKE_SOURCE_DIR}/managed_components")\n'
            'endif()\n'
            'include("${CMAKE_SOURCE_DIR}/scripts/jpeg_observer_guard.cmake")\n'
            'add_subdirectory(components/display)\n')
        self.env = BoundaryEnvironment(self.project, self.build)
        self.graph = self.build / "build.ninja"
        self.stamp = self.build / "cmake_inputs.sha256"
        self.protected = [self.project / "sdkconfig.fixture",
                          self.build / "sdkconfig_inputs.sha256",
                          self.build / "x509_crt_bundle",
                          self.build / "x509_crt_bundle.S"]
        for path in self.protected:
            path.write_bytes(b"preserved SDK artifact\n")

    def hook(self, targets=()):
        script = types.ModuleType("SCons.Script")
        script.COMMAND_LINE_TARGETS = list(targets)
        scons = types.ModuleType("SCons")
        scons.Script = script

        def import_environment(name):
            self.assertEqual(name, "env")

        output = io.StringIO()
        with mock.patch.dict(sys.modules, {"SCons": scons, "SCons.Script": script}), \
                redirect_stdout(output):
            try:
                runpy.run_path(str(HOOK), init_globals={
                    "Import": import_environment, "env": self.env,
                })
            finally:
                self.last_output = output.getvalue()
        return self.last_output

    def configure(self, expect_success=True):
        result = subprocess.run(
            [str(CMAKE), "-S", str(self.project), "-B", str(self.build),
             "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + str(NINJA)],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode == 0, expect_success,
                         result.stdout + result.stderr)
        return result

    def pio_requires_configuration(self):
        """Execute the installed function unchanged; only globals are adapted."""
        source = ESP_IDF_BUILDER.read_text()
        tree = ast.parse(source)
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                        and node.name == "is_cmake_reconfigure_required")
        namespace = {"os": os, "BUILD_DIR": str(self.build),
                     "PROJECT_DIR": str(self.project),
                     "PROJECT_SRC_DIR": str(self.env.source),
                     "SDKCONFIG_PATH": str(self.protected[0]),
                     "FRAMEWORK_DIR": str(self.project / "framework")}
        exec(compile(ast.Module(body=[function], type_ignores=[]),
                     str(ESP_IDF_BUILDER), "exec"), namespace)
        return namespace[function.name](str(self.build / ".cmake/api/v1/reply"))

    def cached_graph(self):
        self.hook()
        self.configure()
        for name in ("config", ".cmake/api/v1/reply"):
            directory = self.build / name
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "fixture").write_text("nonempty\n")
        # Make every PlatformIO-watched input older than the cached graph.
        old = 1_000_000_000
        for path in (self.project / "CMakeLists.txt",
                     self.env.source / "CMakeLists.txt", self.protected[0],
                     self.build / "config", self.project / "framework"):
            os.utime(path, ns=(old, old))
        os.utime(self.graph, ns=(2_000_000_000, 2_000_000_000))
        self.assertFalse(self.pio_requires_configuration())

    def assert_protected(self):
        for path in self.protected:
            self.assertEqual(path.read_bytes(), b"preserved SDK artifact\n")

    def test_stale_component_graph_reproduced_then_reconfigured(self):
        self.cached_graph()
        self.assertNotIn("jpeg_observer.cpp", self.graph.read_text())
        self.cmake_file.write_text(
            'add_custom_target(display_fixture DEPENDS display_port.cpp jpeg_observer.cpp)\n')
        self.assertFalse(self.pio_requires_configuration())
        self.hook()
        self.assertFalse(self.graph.exists())
        self.assertTrue(self.pio_requires_configuration())
        self.configure()
        self.assertIn("jpeg_observer.cpp", self.graph.read_text())
        self.assert_protected()

    def test_unchanged_inputs_preserve_graph_and_fingerprint(self):
        self.cached_graph()
        for path in (self.graph, self.stamp):
            os.utime(path, ns=(3_000_000_000, 3_000_000_000))
        before = [(path.read_bytes(), path.stat().st_mtime_ns)
                  for path in (self.graph, self.stamp)]
        self.assertEqual(self.hook(), "")
        self.assertEqual(before, [(path.read_bytes(), path.stat().st_mtime_ns)
                                 for path in (self.graph, self.stamp)])
        self.assert_protected()

    def test_owned_cmake_inputs_and_version_each_invalidate_graph(self):
        for relative in ("CMakeLists.txt", "src/CMakeLists.txt",
                         "components/display/CMakeLists.txt",
                         "scripts/jpeg_observer_guard.cmake", "VERSION"):
            with self.subTest(input=relative):
                self.hook()
                self.graph.write_text("cached graph\n")
                path = self.project / relative
                original = path.read_bytes()
                path.write_bytes(original + b"\n")
                self.hook()
                self.assertFalse(self.graph.exists())
                self.assert_protected()
                path.write_bytes(original)

    def test_component_and_helper_additions_and_removals_are_inputs(self):
        for relative in ("components/new/nested/CMakeLists.txt", "scripts/new.cmake"):
            with self.subTest(input=relative):
                self.hook()
                self.graph.write_text("cached graph\n")
                path = self.project / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("# New owned CMake input.\n")
                self.hook()
                self.assertFalse(self.graph.exists())
                self.graph.write_text("cached graph after addition\n")
                path.unlink()
                self.hook()
                self.assertFalse(self.graph.exists())
                self.assert_protected()

    def test_each_guarded_source_drift_aborts_before_cache_mutation(self):
        self.cached_graph()
        before = self.graph.read_bytes(), self.stamp.read_bytes()
        for relative in self.contracts:
            with self.subTest(source=relative):
                path = self.lvgl / relative
                original = path.read_bytes()
                path.write_bytes(original + b"\n")
                with self.assertRaises(SystemExit) as error:
                    self.hook()
                self.assertEqual(error.exception.code, 1)
                self.assertIn("changed; review", self.last_output)
                self.assertEqual(before, (self.graph.read_bytes(), self.stamp.read_bytes()))
                self.assert_protected()
                path.write_bytes(original)

    def test_partial_managed_installation_is_fatal(self):
        self.cached_graph()
        before = self.graph.read_bytes(), self.stamp.read_bytes()
        (self.lvgl / self.contracts[0]).unlink()
        with self.assertRaises(SystemExit):
            self.hook()
        self.assertIn("JPEG observer: missing", self.last_output)
        self.assertEqual(before, (self.graph.read_bytes(), self.stamp.read_bytes()))
        self.assert_protected()

    def test_absent_managed_directory_forces_verified_bootstrap(self):
        self.cached_graph()
        resolved = self.project / "resolved"
        resolved.mkdir()
        shutil.move(self.lvgl, resolved / "lvgl__lvgl")
        # Same fingerprint must not allow a pre-existing graph to bypass resolution.
        self.assertIn("LVGL bootstrap", self.hook())
        self.assertFalse(self.graph.exists())
        self.graph.write_text("unverified graph\n")
        self.hook()
        self.assertFalse(self.graph.exists())
        self.configure()
        self.assertTrue(self.lvgl.is_dir())
        current = self.graph.read_bytes(), self.graph.stat().st_mtime_ns
        self.assertEqual(self.hook(), "")
        self.assertEqual(current, (self.graph.read_bytes(), self.graph.stat().st_mtime_ns))
        self.assert_protected()

    def test_bootstrap_configuration_rejects_drift_before_graph(self):
        resolved = self.project / "resolved"
        resolved.mkdir()
        shutil.move(self.lvgl, resolved / "lvgl__lvgl")
        path = resolved / "lvgl__lvgl" / self.contracts[-1]
        path.write_bytes(path.read_bytes() + b"\n")
        self.hook()
        result = self.configure(expect_success=False)
        self.assertIn("changed; review", result.stderr)
        self.assertFalse(self.graph.exists())
        self.assert_protected()

    def test_maintenance_only_targets_do_not_require_tools_or_sources(self):
        shutil.rmtree(self.lvgl)
        self.env.cmake_package = None
        for targets in (("clean",), ("fullclean",), ("erase",), ("monitor",),
                        ("menuconfig",), ("clean", "monitor")):
            with self.subTest(targets=targets):
                self.assertEqual(self.hook(targets), "")
                self.assertEqual(self.env.package_reads, 0)
                self.assertFalse(self.stamp.exists())

    def test_mixed_build_and_maintenance_targets_still_check_contract(self):
        (self.lvgl / self.contracts[0]).write_text("drift\n")
        with self.assertRaises(SystemExit):
            self.hook(("clean", "buildprog"))
        self.assertIn("changed; review", self.last_output)

    def test_scons_clean_option_with_empty_targets_skips_then_build_checks(self):
        (self.lvgl / self.contracts[0]).write_text("drift\n")
        self.env.cmake_package = None
        self.env.clean = True
        self.assertEqual(self.hook(), "")
        self.assertEqual(self.env.package_reads, 0)
        self.assertFalse(self.stamp.exists())
        self.env.clean = False
        with self.assertRaises(SystemExit):
            self.hook(("buildprog",))
        self.assertIn("tool-cmake package not found", self.last_output)

    def test_missing_cmake_tool_stops_before_mutation(self):
        self.cached_graph()
        before = self.graph.read_bytes(), self.stamp.read_bytes()
        self.env.cmake_package = None
        with self.assertRaises(SystemExit):
            self.hook()
        self.assertIn("tool-cmake package not found", self.last_output)
        self.assertEqual(before, (self.graph.read_bytes(), self.stamp.read_bytes()))

    def test_platformio_wires_pre_guard_before_config_guard_and_excludes_native(self):
        config = configparser.ConfigParser()
        config.read(ROOT / "platformio.ini")
        scripts = config["env"]["extra_scripts"].split()
        self.assertEqual(scripts.count("pre:scripts/cmake_guard.py"), 1)
        self.assertLess(scripts.index("pre:scripts/cmake_guard.py"),
                        scripts.index("pre:scripts/sdkconfig_guard.py"))
        self.assertEqual(config["env:native"]["extra_scripts"],
                         "pre:scripts/native_sources.py")

    def test_failed_configuration_still_requires_reconfiguration(self):
        self.cached_graph()
        self.cmake_file.write_text('message(FATAL_ERROR "injected configure failure")\n')
        self.hook()
        self.configure(expect_success=False)
        self.assertFalse(self.graph.exists())
        self.hook()
        self.assertTrue(self.pio_requires_configuration())
        self.assert_protected()


if __name__ == "__main__":
    unittest.main()
