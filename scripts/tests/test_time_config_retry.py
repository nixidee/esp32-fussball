#!/usr/bin/env python3
"""Actual time owner: injected configuration survives failed TZ allocation."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="clock-config-") as directory:
    folder = Path(directory)
    (folder / "freertos").mkdir()
    (folder / "esp_err.h").write_text("#pragma once\nusing esp_err_t=int; inline constexpr int ESP_OK=0,ESP_FAIL=-1;\n")
    (folder / "esp_log.h").write_text('''#pragma once
template<class... T> void hostLog(const T&...) {}
#define ESP_LOGI(...) hostLog(__VA_ARGS__)
#define ESP_LOGE(...) hostLog(__VA_ARGS__)
#define ESP_LOGW(...) hostLog(__VA_ARGS__)
''')
    (folder / "esp_timer.h").write_text("#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n")
    (folder / "freertos/FreeRTOS.h").write_text('''#pragma once
#include <cassert>
struct StaticSemaphore_t { bool held=false; };
using SemaphoreHandle_t=StaticSemaphore_t*;
inline constexpr unsigned portMAX_DELAY=~0u;
inline auto xSemaphoreCreateMutexStatic(StaticSemaphore_t* p){return p;}
inline void xSemaphoreTake(SemaphoreHandle_t p,unsigned){assert(!p->held);p->held=true;}
inline void xSemaphoreGive(SemaphoreHandle_t p){assert(p->held);p->held=false;}
''')
    (folder / "freertos/semphr.h").write_text('#include "FreeRTOS.h"\n')
    (folder / "main.cpp").write_text('''#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "time_service.h"
#include "event_bus.h"
int64_t now_ms=0;
unsigned attempts=0,notifications=0;
bool fail=false;
int injectedSetenv(const char* key,const char* value,int replace) {
  ++attempts;
  return fail ? -1 : ::setenv(key,value,replace);
}
int64_t esp_timer_get_time(){return now_ms*1000;}
namespace events {esp_err_t post(Event e){assert(e==Event::kTimeChanged);++notifications;return ESP_OK;}}
#define setenv injectedSetenv
#include "components/timekeeping/time_service.cpp"
#undef setenv
int main(){
  using namespace timekeeping;
  const auto& a=cfg::kTimeZones[0];const auto& b=cfg::kTimeZones[1];
  const auto& c=cfg::kTimeZones[2];
  init(a);assert(attempts==1&&notifications==1);
  assert(std::strcmp(std::getenv("TZ"),a.rule)==0);
  configureZone(a);assert(attempts==1&&notifications==1);
  fail=true;configureZone(b);assert(attempts==2&&notifications==1);
  assert(std::strcmp(std::getenv("TZ"),a.rule)==0);
  now_ms=cfg::kTimeZoneRetryMs-1;poll();assert(attempts==2);
  now_ms++;poll();assert(attempts==3&&notifications==1);
  configureZone(c);assert(attempts==4);
  fail=false;now_ms+=cfg::kTimeZoneRetryMs;poll();
  assert(attempts==5&&notifications==2);
  assert(std::strcmp(std::getenv("TZ"),c.rule)==0);
  now_ms+=cfg::kTimeZoneRetryMs;poll();assert(attempts==5);
  internal::applyZone(a);internal::restoreRequestedZone();
  assert(std::strcmp(std::getenv("TZ"),c.rule)==0);
  std::cout<<"PASS actual clock owner: unchanged config, failed allocation, timed retry, superseding zone, test restore\\n";
}
''')
    command = ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-I" + str(folder), "-iquote", str(root), "-I" + str(root / "include")]
    command += ["-I" + str(root / "components" / owner / "include")
                for owner in ("timekeeping", "events")]
    binary = folder / "check"
    subprocess.run(command + [str(folder / "main.cpp"),
                              str(root / "components/timekeeping/civil_time.cpp"),
                              "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
