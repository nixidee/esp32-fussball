#!/usr/bin/env python3
"""Whole image/files/JPEG owners against real POSIX/decoder and SDK faults.

Only SDK/POSIX/LVGL cache boundaries are adapted. HTTP receipt extraction is
retained until its separate whole-transport owner cutover; no renderer exists.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
decoder = ROOT / "managed_components/lvgl__lvgl/src/libs/tjpgd"
with tempfile.TemporaryDirectory(prefix="im", dir="/tmp") as tmp:
    folder = Path(tmp)
    (folder / "esp_err.h").write_text('''#pragma once
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_ARG=1,
 ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_INVALID_SIZE=4,
 ESP_ERR_NOT_FOUND=5,ESP_ERR_TIMEOUT=6,ESP_ERR_INVALID_RESPONSE=7;
''')
    (folder / "image_sdk.h").write_text('''#pragma once
#include <cstdint>
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(value) ((void)(value))
#define portEXIT_CRITICAL(value) ((void)(value))
template<class... T> void imageLog(const T&...){}
#define ESP_LOGI(...) imageLog(__VA_ARGS__)
#define ESP_LOGW(...) imageLog(__VA_ARGS__)
int64_t esp_timer_get_time();uint32_t esp_random();
''')
    for header in ("esp_log.h", "esp_timer.h", "esp_random.h", "freertos/FreeRTOS.h"):
        target = folder / header
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('#pragma once\n#include "image_sdk.h"\n')
    (folder / "sdkconfig.h").write_text("#pragma once\n#define CONFIG_IDF_TARGET_ESP32C6 1\n")
    (folder / "hw_target.h").write_text('#pragma once\n#include "targets/xiao_esp32c6_gc9a01.h"\n')
    (folder / "lvgl.h").write_text('#pragma once\nvoid lv_image_cache_drop(const char*);\n')
    cache = folder / "src/misc/cache/instance/lv_image_cache.h"
    cache.parent.mkdir(parents=True)
    cache.write_text('#pragma once\nvoid lv_image_cache_drop(const char*);\n')
    # Preserve the installed C decoder/config. Adapt only its LVGL public include.
    header = (decoder / "tjpgd.h").read_text().replace(
        '#include "../../lvgl_public.h"',
        '#define LV_USE_TJPGD 1\n#define LV_STDINT_INCLUDE <stdint.h>')
    nested = folder / "src/libs/tjpgd"
    nested.mkdir(parents=True)
    for base in (folder, nested):
        (base / "tjpgd.h").write_text(header)
        (base / "tjpgdcnf.h").write_bytes((decoder / "tjpgdcnf.h").read_bytes())
    (folder / "tjpgd.c").write_bytes((decoder / "tjpgd.c").read_bytes())
    decoder_object = folder / "jpeg.o"
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-Djd_prepare=lvgl_jd_prepare",
                    "-Djd_decomp=lvgl_jd_decomp", "-I" + str(folder), "-c",
                    str(folder / "tjpgd.c"), "-o", str(decoder_object)], check=True)
    common = '''#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstddef>
int test_open(const char*,int,int=0);
ssize_t test_read(int,void*,std::size_t);
ssize_t test_write(int,const void*,std::size_t);
int test_sync(int);int test_close(int);
int test_rename(const char*,const char*);int test_unlink(const char*);
int test_stat(const char*,struct stat*);
#define stat(...) test_stat(__VA_ARGS__)
#define unlink(...) test_unlink(__VA_ARGS__)
'''
    (folder / "image_faults.h").write_text(common)
    (folder / "file_faults.h").write_text(common + '''
#define open(...) test_open(__VA_ARGS__)
#define read(...) test_read(__VA_ARGS__)
#define write(...) test_write(__VA_ARGS__)
#define close(...) test_close(__VA_ARGS__)
#define fsync(...) test_sync(__VA_ARGS__)
#define rename(...) test_rename(__VA_ARGS__)
''')
    (folder / "jpeg_faults.h").write_text('''#pragma once
#include <cstdio>
FILE* test_fopen(const char*,const char*);
size_t test_fread(void*,size_t,size_t,FILE*);
int test_ferror(FILE*);int test_fseek(FILE*,long,int);
long test_ftell(FILE*);int test_fclose(FILE*);
#define fopen(...) test_fopen(__VA_ARGS__)
#define fread(...) test_fread(__VA_ARGS__)
#define ferror(...) test_ferror(__VA_ARGS__)
#define fseek(...) test_fseek(__VA_ARGS__)
#define ftell(...) test_ftell(__VA_ARGS__)
#define fclose(...) test_fclose(__VA_ARGS__)
''')
    includes = [folder, ROOT / "include",
                ROOT / ".pio/libdeps/native/ArduinoJson/src", ROOT / "components/web"]
    includes.extend((ROOT / "components").glob("*/include"))
    command = ["clang++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command.extend("-I" + str(path) for path in includes)
    command += ["-iquote", str(ROOT)]
    objects = [decoder_object]
    for name, source, faults in (
        ("images", ROOT / "components/images/image_service.cpp", "image_faults.h"),
        ("candidate", ROOT / "components/files/file_candidate.cpp", "file_faults.h"),
        ("io", ROOT / "components/files/file_io.cpp", "file_faults.h"),
        ("validation", ROOT / "components/display/jpeg_validation.cpp", "jpeg_faults.h")):
        target = folder / (name + ".o")
        subprocess.run(command + ["-include", str(folder / faults), "-c", str(source),
                                   "-o", str(target)], check=True)
        objects.append(target)
    # Existing HTTP receipt tests use their original production handler bodies.
    fixture = (ROOT / "scripts/tests/fixtures/image_files_host.cpp").read_text()
    web = (ROOT / "components/web/web_service.cpp").read_text()
    first, last = web.index("struct Fault {"), web.index("class Writer {")
    fixture = fixture.replace("// HTTP_FAULTS", web[first:last])
    first = web.index("constexpr const char* kStockNames[]")
    last = web.index("esp_err_t getImageFile(", first)
    delete = web.index("esp_err_t deleteImage(httpd_req_t* req)")
    end = web.index("esp_err_t postImageRole(", delete)
    fixture = fixture.replace("// DELETE_HANDLER", web[first:last] + web[delete:end])
    (folder / "main.cpp").write_text(fixture)
    codec = (ROOT / "components/web/request_json.cpp").read_text()
    prefix = codec[:codec.index("SettingsDecode readDemo(")]
    first = codec.index("SettingsDecode readImageId(")
    last = codec.index("SettingsDecode readImageRole(", first)
    (folder / "request.cpp").write_text(prefix + codec[first:last] + "\n} // namespace web\n")
    binary = folder / "check"
    subprocess.run(command + list(map(str, [folder / "main.cpp", folder / "request.cpp",
                       ROOT / "components/json/json_stream.cpp",
                       ROOT / "components/files/file_names.cpp", *objects])) +
                   ["-o", str(binary)], check=True)
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = "detect_leaks=" + ("0" if sys.platform == "darwin" else "1") + ":abort_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    runtime = folder / "fs"
    runtime.mkdir()
    subprocess.run([str(binary), str(runtime), str(ROOT / "data/default_crest.jpg")],
                   check=True, env=environment)
