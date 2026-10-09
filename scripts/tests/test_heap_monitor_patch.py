"""Focused SDK-guard, CMake-substitution and native error-path checks."""

import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "heap_monitor_patch.py"
CMAKE_HELPER = SCRIPT.with_suffix(".cmake")
SPEC = importlib.util.spec_from_file_location("heap_monitor_patch", SCRIPT)
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)

# Relevant start function from the guarded ESP-IDF 6.1.0 source.
# SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
START_FUNCTION = """esp_err_t heap_caps_monitor_local_minimum_free_size_start(void)
{
    heap_t *heap = NULL;
    MULTI_HEAP_LOCK(&min_free_bytes_monitoring.mux);
    if (min_free_bytes_monitoring.values == NULL) {
        SLIST_FOREACH(heap, &registered_heaps, next) {
            min_free_bytes_monitoring.counter++;
        }
""" + PATCH.ALLOCATION_ANCHOR + """        memset(min_free_bytes_monitoring.values, 0xFF, sizeof(size_t) * min_free_bytes_monitoring.counter);
    }

    heap = SLIST_FIRST(&registered_heaps);
    for (size_t counter = 0; counter < min_free_bytes_monitoring.counter; counter++) {
        if (heap->heap != NULL) {
            size_t old_minimum = multi_heap_reset_minimum_free_bytes(heap->heap);
            if (min_free_bytes_monitoring.values[counter] > old_minimum) {
                min_free_bytes_monitoring.values[counter] = old_minimum;
            }
        }
        heap = SLIST_NEXT(heap, next);
    }
    MULTI_HEAP_UNLOCK(&min_free_bytes_monitoring.mux);
    return ESP_OK;
}
"""
SOURCE = (PATCH.INCLUDE_ANCHOR + START_FUNCTION).encode()

C_HARNESS = """
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define MALLOC_CAP_DEFAULT 0
struct heap { struct heap *next; void *heap; };
typedef struct heap heap_t;
static struct heap tail = {NULL, (void *)1};
static struct heap head = {&tail, (void *)1};
static struct heap *registered_heaps = &head;
#define SLIST_FOREACH(item, list, field) for (item = *(list); item != NULL; item = item->field)
#define SLIST_FIRST(list) (*(list))
#define SLIST_NEXT(item, field) ((item)->field)
static struct { size_t *values; size_t counter; int mux; } min_free_bytes_monitoring;
static int lock_depth;
static int malloc_calls;
static int reset_calls;
static bool malloc_failure;
bool hook_failure;
#define MULTI_HEAP_LOCK(lock) do { (void)(lock); ++lock_depth; } while (0)
#define MULTI_HEAP_UNLOCK(lock) do { (void)(lock); --lock_depth; assert(lock_depth == 0); } while (0)
static void *heap_caps_malloc(size_t size, int caps) {
    (void)caps;
    ++malloc_calls;
    return malloc_failure ? NULL : malloc(size);
}
static size_t multi_heap_reset_minimum_free_bytes(void *heap) {
    (void)heap;
    ++reset_calls;
    return 42;
}
"""
C_HOOK = """
#include <stdbool.h>
extern bool hook_failure;
bool fussball_heap_monitor_fail_allocation(void) {
    bool fail = hook_failure;
    hook_failure = false;
    return fail;
}
"""


class HeapMonitorPatchTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="heap-patch-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sdk = self.root / "sdk"
        self.build = self.root / "build"
        (self.sdk / "tools/cmake").mkdir(parents=True)
        (self.sdk / "components/heap").mkdir(parents=True)
        self.version = self.sdk / "tools/cmake/version.cmake"
        self.version.write_text("set(IDF_VERSION_MAJOR 6)\n"
                                "set(IDF_VERSION_MINOR 1)\n"
                                "set(IDF_VERSION_PATCH 0)\n")
        self.original = self.sdk / "components/heap/heap_caps.c"
        self.original.write_bytes(SOURCE)
        self.digest = hashlib.sha256(SOURCE).hexdigest()
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(PATCH, "EXPECTED_SOURCE_SHA256", self.digest).start()

    def test_generates_local_copy_and_preserves_sdk(self):
        output, changed = PATCH.prepare_patch(self.sdk, self.build)
        self.assertTrue(changed)
        self.assertEqual(self.original.read_bytes(), SOURCE)
        generated = output.read_text()
        self.assertIn(PATCH.ALLOCATION_REPLACEMENT, generated)
        self.assertIn(PATCH.HOOK_DECLARATION, generated)
        self.assertNotIn(PATCH.ALLOCATION_ANCHOR, generated)
        self.assertLess(generated.index("return ESP_ERR_NO_MEM"),
                        generated.index("memset(min_free_bytes_monitoring.values"))

    def test_identical_output_keeps_mtime_and_repairs_modified_copy(self):
        output, _ = PATCH.prepare_patch(self.sdk, self.build)
        os.utime(output, ns=(123456789000000000, 123456789000000000))
        before = output.stat().st_mtime_ns
        self.assertFalse(PATCH.prepare_patch(self.sdk, self.build)[1])
        self.assertEqual(output.stat().st_mtime_ns, before)
        output.write_text("modified generated file")
        self.assertTrue(PATCH.prepare_patch(self.sdk, self.build)[1])
        self.assertIn(PATCH.ALLOCATION_REPLACEMENT, output.read_text())

    def test_cached_copy_does_not_bypass_source_hash_guard(self):
        output, _ = PATCH.prepare_patch(self.sdk, self.build)
        before = output.read_bytes()
        self.original.write_bytes(SOURCE + b"\n")
        with self.assertRaisesRegex(PATCH.PatchError, "SHA256"):
            PATCH.prepare_patch(self.sdk, self.build)
        self.assertEqual(output.read_bytes(), before)

    def test_version_mismatch_writes_nothing(self):
        self.version.write_text("set(IDF_VERSION_MAJOR 6)\n"
                                "set(IDF_VERSION_MINOR 2)\n"
                                "set(IDF_VERSION_PATCH 0)\n")
        with self.assertRaisesRegex(PATCH.PatchError, "found 6.2.0"):
            PATCH.prepare_patch(self.sdk, self.build)
        self.assertFalse(self.build.exists())

    def test_ambiguous_or_missing_version_is_rejected(self):
        for text in ("", self.version.read_text() + "set(IDF_VERSION_MINOR 1)\n"):
            self.version.write_text(text)
            with self.assertRaisesRegex(PATCH.PatchError, "exact ESP-IDF version"):
                PATCH.prepare_patch(self.sdk, self.build)

    def test_anchors_must_each_appear_exactly_once(self):
        for source in (
            SOURCE.decode().replace(PATCH.INCLUDE_ANCHOR, ""),
            SOURCE.decode() + PATCH.ALLOCATION_ANCHOR,
        ):
            with self.assertRaisesRegex(PATCH.PatchError, "exactly one"):
                PATCH.patch_source(source)

    def compile_and_run(self, main, with_hook=False):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("native C compiler unavailable")
        generated = PATCH.patch_source(SOURCE.decode())
        generated = generated.replace(PATCH.INCLUDE_ANCHOR, "", 1)
        if sys.platform == "darwin":
            # Mach-O uses weak_import for the optional undefined import that
            # ELF expresses with weak. Keep the tested branch/assertions intact.
            generated = generated.replace("__attribute__((weak))",
                                          "__attribute__((weak_import))")
        source = self.root / "probe.c"
        source.write_text(C_HARNESS + generated + main)
        argv = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(source)]
        if with_hook:
            hook = self.root / "hook.c"
            hook.write_text(C_HOOK)
            argv.append(str(hook))
        if sys.platform == "darwin" and not with_hook:
            # Darwin's executable linker also needs optional imports to use
            # dynamic lookup; weak_import then resolves a missing hook to NULL.
            argv.append("-Wl,-undefined,dynamic_lookup")
        executable = self.root / "probe"
        argv += ["-o", str(executable)]
        result = subprocess.run(argv, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_normal_link_needs_no_hook_definition(self):
        self.compile_and_run("""
int main(void) {
    assert(heap_caps_monitor_local_minimum_free_size_start() == ESP_OK);
    assert(lock_depth == 0 && malloc_calls == 1 && reset_calls == 2);
    assert(min_free_bytes_monitoring.counter == 2);
    assert(heap_caps_monitor_local_minimum_free_size_start() == ESP_OK);
    assert(malloc_calls == 1 && reset_calls == 4);
    free(min_free_bytes_monitoring.values);
    return 0;
}
""")

    def test_real_null_allocation_returns_and_retry_is_clean(self):
        self.compile_and_run("""
int main(void) {
    malloc_failure = true;
    assert(heap_caps_monitor_local_minimum_free_size_start() == ESP_ERR_NO_MEM);
    assert(lock_depth == 0 && malloc_calls == 1 && reset_calls == 0);
    assert(min_free_bytes_monitoring.values == NULL);
    assert(min_free_bytes_monitoring.counter == 0);
    malloc_failure = false;
    assert(heap_caps_monitor_local_minimum_free_size_start() == ESP_OK);
    assert(lock_depth == 0 && malloc_calls == 2 && reset_calls == 2);
    assert(min_free_bytes_monitoring.counter == 2);
    free(min_free_bytes_monitoring.values);
    return 0;
}
""")

    def test_strong_hook_only_fails_the_monitor_allocation(self):
        self.compile_and_run("""
int main(void) {
    hook_failure = true;
    assert(heap_caps_monitor_local_minimum_free_size_start() == ESP_ERR_NO_MEM);
    assert(!hook_failure);
    assert(lock_depth == 0 && malloc_calls == 0 && reset_calls == 0);
    assert(min_free_bytes_monitoring.values == NULL);
    assert(min_free_bytes_monitoring.counter == 0);
    assert(heap_caps_monitor_local_minimum_free_size_start() == ESP_OK);
    assert(lock_depth == 0 && malloc_calls == 1 && reset_calls == 2);
    assert(min_free_bytes_monitoring.counter == 2);
    hook_failure = true;
    assert(heap_caps_monitor_local_minimum_free_size_start() == ESP_OK);
    assert(hook_failure && malloc_calls == 1 && reset_calls == 4);
    free(min_free_bytes_monitoring.values);
    return 0;
}
""", with_hook=True)

    def run_cmake(self, sources, abort=False, generate=True):
        cmake = shutil.which("cmake")
        bundled = Path.home() / ".platformio/packages/tool-cmake/bin/cmake"
        if cmake is None and bundled.is_file():
            cmake = str(bundled)
        if cmake is None:
            self.skipTest("CMake unavailable")
        if generate:
            PATCH.prepare_patch(self.sdk, self.build)
        other = self.sdk / "components/heap/other.c"
        other.write_text("int other;\n")
        original = self.original.as_posix()
        entries = {"original": original, "other": other.as_posix()}
        source_list = " ".join('"%s"' % entries[s] for s in sources)
        project = self.root / "fixture"
        project.mkdir()
        (project / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.22)\n'
            'project(heap_patch_fixture C)\n'
            'add_library(heap_fixture STATIC %s)\n' % source_list +
            'function(idf_component_get_property out component property)\n'
            '  if(property STREQUAL "COMPONENT_LIB")\n'
            '    set(${out} heap_fixture PARENT_SCOPE)\n'
            '  else()\n'
            '    set(${out} "%s" PARENT_SCOPE)\n' % self.original.parent.as_posix() +
            '  endif()\n'
            'endfunction()\n'
            'set(CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS %s)\n' % ("ON" if abort else "OFF") +
            'include("%s")\n' % CMAKE_HELPER.as_posix() +
            'get_target_property(result heap_fixture SOURCES)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "${result}")\n'
            'get_target_property(result heap_fixture INCLUDE_DIRECTORIES)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/includes.txt" "${result}")\n'
        )
        return subprocess.run([cmake, "-S", str(project), "-B", str(self.build)],
                              capture_output=True, text=True)

    def test_cmake_replaces_one_source_and_keeps_other_sources(self):
        result = self.run_cmake(["original", "other"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.build / "sources.txt").read_text().split(";"),
                         [str(self.build / PATCH.OUTPUT_RELATIVE_PATH),
                          str(self.original.parent / "other.c")])
        self.assertIn(str(self.original.parent),
                      (self.build / "includes.txt").read_text().split(";"))

    def test_cmake_rejects_multiple_original_sources(self):
        result = self.run_cmake(["original", "original"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("found 2", result.stdout + result.stderr)

    def test_cmake_rejects_missing_original_source(self):
        result = self.run_cmake(["other"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("found 0", result.stdout + result.stderr)

    def test_cmake_rejects_global_allocation_aborts(self):
        result = self.run_cmake(["original"], abort=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("allocation aborts", result.stdout + result.stderr)

    def test_cmake_rejects_missing_generated_copy(self):
        result = self.run_cmake(["original"], generate=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing local heap source", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
