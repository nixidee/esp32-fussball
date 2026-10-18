#!/usr/bin/env python3
"""Real POSIX image publication + actual installed JPEG decoder, host faults.

Adapters replace display locks, NVS-cleanup boundary, time and mount admission.
Production image/JPEG/file-I/O bodies and checked-in factory JPEG are exercised.
No device, network, preset, dependency or partition access is required.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
decoder = ROOT / "managed_components/lvgl__lvgl/src/libs/tjpgd"
with tempfile.TemporaryDirectory(prefix="i", dir="/tmp") as tmp:
    folder = Path(tmp)
    (folder / "esp_err.h").write_text('''#pragma once
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_ARG=1,
 ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_INVALID_SIZE=4,
 ESP_ERR_NOT_FOUND=5,ESP_ERR_TIMEOUT=6,ESP_ERR_INVALID_RESPONSE=7;
''')
    # Only the LVGL public/config include is adapted; decoder code/config stay
    # byte-identical to the installed dependency. No renderer is implemented.
    header = (decoder / "tjpgd.h").read_text().replace(
        '#include "../../lvgl_public.h"',
        '#define LV_USE_TJPGD 1\n#define LV_STDINT_INCLUDE <stdint.h>')
    (folder / "tjpgd.h").write_text(header)
    (folder / "tjpgdcnf.h").write_bytes((decoder / "tjpgdcnf.h").read_bytes())
    (folder / "tjpgd.c").write_bytes((decoder / "tjpgd.c").read_bytes())
    decoder_object = folder / "jpeg.o"
    subprocess.run(["cc", "-std=c11", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I" + str(folder), "-c", str(folder / "tjpgd.c"),
                    "-o", str(decoder_object)], check=True)
    fixture = (ROOT / "scripts/tests/fixtures/image_files_host.cpp").read_text()
    fixture = fixture.replace('#include "file_io.h"',
                              '#include "file_io.h"\n#include "file_service.h"')
    fixture = fixture.replace('// IMAGE_SERVICE',
                              'void lv_image_cache_drop(const char*){}\n// IMAGE_SERVICE')
    service = (ROOT / "components/images/image_service.cpp").read_text()
    service = service[service.index("namespace images {"):]
    service = service.replace("cfg::kFsBasePath", "imageTestRoot.c_str()")
    service = service.replace("stat(p, &info)", "test_stat(p, &info)")
    service = service.replace("::open(", "::test_open(")
    service = service.replace("rename(temp, target)", "test_rename(temp, target)")
    service = service.replace("unlink(p)", "test_unlink(p)")
    validator = (ROOT / "components/display/jpeg_validation.cpp").read_text()
    validator = validator[validator.index("namespace display {"):]
    validator = validator.replace("int64_t deadline_ms) {",
                                  "int64_t deadline_ms) {\n  ++validations;")
    for original, replacement in {
        'fopen(': 'test_fopen(', 'fread(': 'test_fread(',
        'ferror(': 'test_ferror(', 'fseek(': 'test_fseek(',
        'ftell(': 'test_ftell(', 'fclose(': 'test_fclose('
    }.items():
        validator = validator.replace(original, replacement)
    fixture = fixture.replace("// IMAGE_SERVICE", service).replace(
        "// JPEG_VALIDATOR", validator)
    web = (ROOT / "components/web/web_service.cpp").read_text()
    first = web.index("struct Fault {")
    last = web.index("class Writer {", first)
    fixture = fixture.replace("// HTTP_FAULTS", web[first:last])
    role_first = web.index("constexpr const char* kStockNames[]")
    role_last = web.index("esp_err_t getImages(", role_first)
    start = web.index("esp_err_t deleteImage(httpd_req_t* req)")
    end = web.index('esp_err_t postImageRole(', start)
    inventory_last = web.index("esp_err_t getImageFile(", role_last)
    fixture = fixture.replace("// DELETE_HANDLER", web[role_first:inventory_last] + web[start:end])

    (folder / "main.cpp").write_text(fixture)
    codec = (ROOT / "components/web/request_json.cpp").read_text()
    prefix = codec[:codec.index("SettingsDecode readDemo(")]
    first = codec.index("SettingsDecode readImageId(")
    last = codec.index("SettingsDecode readImageRole(", first)
    (folder / "request.cpp").write_text(prefix + codec[first:last] + "\n} //namespace web\n")
    io = (ROOT / "components/files/file_io.cpp").read_text()
    io = '#include "faults.h"\n' + io.replace("::read(", "::test_read(").replace(
        "::write(", "::test_write(").replace("::close(", "::test_close(").replace(
        "fsync(fd)", "test_sync(fd)")
    (folder / "io.cpp").write_text(io)
    (folder / "faults.h").write_text('''#include <sys/types.h>
#include <cstddef>
ssize_t test_read(int,void*,std::size_t);
ssize_t test_write(int,const void*,std::size_t);
int test_sync(int);int test_close(int);
''')
    includes = [folder, ROOT / "include",
                ROOT / ".pio/libdeps/native/ArduinoJson/src", ROOT / "components/web"]
    includes.extend((ROOT / "components").glob("*/include"))
    command = ["c++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command.extend("-I" + str(path) for path in includes)
    command.extend(map(str, [folder / "main.cpp", folder / "io.cpp",
                             folder / "request.cpp", ROOT / "components/json/json_stream.cpp",
                             ROOT / "components/files/file_names.cpp",
                             decoder_object]))
    binary = folder / "check"
    command += ["-o", str(binary)]
    subprocess.run(command, check=True)
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = "detect_leaks=" + ("0" if sys.platform == "darwin" else "1") + ":abort_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    # Keep the real production path bound even for the longer factory name.
    while True:
        runtime = Path("/tmp") / ("f" + os.urandom(3).hex())
        try:
            runtime.mkdir()
            break
        except FileExistsError:
            pass
    try:
        subprocess.run([str(binary), str(runtime), str(ROOT / "data/default_crest.jpg")],
                       check=True, env=environment)
    finally:
        shutil.rmtree(runtime)
