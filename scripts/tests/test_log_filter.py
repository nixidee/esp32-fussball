#!/usr/bin/env python3
"""Compare actual SDK runtime filtering with/without the unused tag cache.

Call the runtime guard directly as a separately compiled library can. Product
diagnostic controls toggle sampling, not tag levels; the sole linked setters
are the SDK WiFi constructor setting four tags to the existing INFO default.
"""
from pathlib import Path
import subprocess
import tempfile

sdk = Path.home() / ".platformio/packages/framework-espidf/components"
log = sdk / "log"
root = Path(__file__).resolve().parents[2]
defaults = (root / "sdkconfig.defaults").read_text()
assert "CONFIG_LOG_DEFAULT_LEVEL_INFO=y" in defaults
assert "CONFIG_LOG_TAG_LEVEL_IMPL_NONE=y" in defaults
assert "CONFIG_LOG_DYNAMIC_LEVEL_CONTROL=y" in defaults
wifi = (sdk / "esp_wifi/src/wifi_init.c").read_text()
for tag in ("wifi", "mesh", "smartconfig", "ESPNOW"):
    assert f'esp_log_level_set("{tag}", CONFIG_LOG_DEFAULT_LEVEL);' in wifi

with tempfile.TemporaryDirectory(prefix="log-filter-") as directory:
    folder = Path(directory)
    (folder / "esp_private").mkdir()
    (folder / "esp_attr.h").write_text("")
    (folder / "esp_assert.h").write_text('#define ESP_STATIC_ASSERT _Static_assert\n')
    (folder / "esp_compiler.h").write_text('#define __containerof(ptr,type,member) ((type*)((char*)(ptr)-offsetof(type,member)))\n')
    (folder / "esp_err.h").write_text('typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM 0x101\n')
    # Host-only RTOS boundary. Real SDK filtering/cache/list code is compiled.
    (folder / "esp_private/log_lock.h").write_text('''
#include <stdbool.h>
static inline void esp_log_impl_lock(void) {}
static inline void esp_log_impl_unlock(void) {}
static inline bool esp_log_impl_lock_timeout(void) {return true;}
''')
    (folder / "check.c").write_text('''
#include <assert.h>
#include <stdio.h>
#include "esp_log_level.h"
#include "esp_private/log_level.h"
int main(void) {
 const char* tags[]={"wifi","mesh","smartconfig","ESPNOW","health","display","unknown",NULL};
 for(unsigned i=0;i<4;++i) esp_log_level_set(tags[i],CONFIG_LOG_DEFAULT_LEVEL);
 assert(esp_log_get_default_level()==ESP_LOG_INFO);
 for(unsigned i=0;i<sizeof(tags)/sizeof(tags[0]);++i) {
  assert(esp_log_level_get(tags[i])==ESP_LOG_INFO);
  for(unsigned level=ESP_LOG_NONE;level<=ESP_LOG_VERBOSE;++level) {
   // Same mandatory NONE check as actual esp_log_va before the runtime guard.
   const bool emitted=level!=ESP_LOG_NONE && esp_log_is_tag_loggable(level,tags[i]);
   assert(emitted==(level>=ESP_LOG_ERROR && level<=ESP_LOG_INFO));
   putchar(emitted?'1':'0');
  }
 }
 putchar('\n');
 esp_log_level_set("*",ESP_LOG_WARN);
 assert(!esp_log_is_tag_loggable(ESP_LOG_INFO,"unknown"));
 assert(esp_log_is_tag_loggable(ESP_LOG_WARN,"unknown"));
 esp_log_level_set("*",ESP_LOG_INFO);
 return 0;
}
'''.replace("putchar('\n');", "putchar('\\n');"))
    outputs = []
    for cache in (True, False):
        (folder / "sdkconfig.h").write_text(f'''
#define CONFIG_LOG_DEFAULT_LEVEL 3
#define CONFIG_LOG_MAXIMUM_LEVEL 3
#define CONFIG_LOG_DYNAMIC_LEVEL_CONTROL 1
#define CONFIG_LOG_TAG_LEVEL_IMPL_NONE {int(not cache)}
#define CONFIG_LOG_TAG_LEVEL_IMPL_CACHE_AND_LINKED_LIST {int(cache)}
#define CONFIG_LOG_TAG_LEVEL_IMPL_LINKED_LIST 0
#define CONFIG_LOG_TAG_LEVEL_CACHE_BINARY_MIN_HEAP {int(cache)}
#define CONFIG_LOG_TAG_LEVEL_CACHE_SIZE 31
#define CONFIG_LOG_TAG_LEVEL_IMPL_CACHE_SIZE 31
''')
        base = log / "src/log_level"
        sources = [base / "log_level.c", base / "tag_log_level/tag_log_level.c"]
        if cache:
            sources += [base / "tag_log_level/cache/log_binary_heap.c",
                        base / "tag_log_level/linked_list/log_linked_list.c"]
        binary = folder / ("cache" if cache else "global")
        subprocess.run(["clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I"+str(folder), "-I"+str(log / "include"),
                        str(folder / "check.c"), *map(str, sources), "-o", str(binary)], check=True)
        outputs.append(subprocess.check_output([str(binary)], text=True))
    assert outputs[0] == outputs[1]
    print("PASS actual SDK cache/global INFO filtering:8tags x6levels, direct library calls, WiFi startup setters, global updates")
