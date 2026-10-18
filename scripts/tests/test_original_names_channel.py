#!/usr/bin/env python3
"""Replay the real bounded channel and adapter with two controlled host tasks.

The RTOS surface is a condition-variable adapter; this proves correlation,
backpressure and cancellation, not target scheduler/runtime timing.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FREERTOS = r'''
#pragma once
#include <cstdint>
#include <mutex>
using TickType_t = uint32_t;
using BaseType_t = int;
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)
'''
EVENTS = r'''
#pragma once
#include <chrono>
#include <condition_variable>
#include "FreeRTOS.h"
using EventBits_t = uint32_t;
struct StaticEventGroup_t {
 std::mutex mutex;
 std::condition_variable wake;
 EventBits_t bits=0;
};
using EventGroupHandle_t = StaticEventGroup_t*;
inline EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t* p){return p;}
inline EventBits_t xEventGroupSetBits(EventGroupHandle_t p,EventBits_t bits){
 std::lock_guard lock(p->mutex); p->bits|=bits;p->wake.notify_all();return p->bits;
}
inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t p,EventBits_t bits,
 BaseType_t clear,BaseType_t,TickType_t ticks){
 std::unique_lock lock(p->mutex);
 p->wake.wait_for(lock,std::chrono::milliseconds(ticks),[&]{return p->bits&bits;});
 const auto result=p->bits;if(clear)p->bits&=~bits;return result;
}
'''
TEST = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include "original_names_channel.h"
#include "original_names_channel_state.h"
std::atomic<int64_t> clock_offset{0};
int64_t esp_timer_get_time(){
 return std::chrono::duration_cast<std::chrono::microseconds>(
 std::chrono::steady_clock::now().time_since_epoch()).count()+clock_offset.load();
}
using namespace football::originals;
settings::Source source(){settings::Source s;s.provider=cfg::Provider::kEspn;
 assert(s.competition.assign("ger.1"));assert(s.season.assign("2026"));return s;}
uint32_t start(){uint32_t id=0;assert(begin(source(),id)==ESP_OK&&id);return id;}
int main(){
 char out[73];settings::Source actual;
 assert(read(123,out,sizeof(out),0).state==State::kStale);
 uint32_t id=99;assert(begin({},id)==ESP_ERR_INVALID_ARG&&id==0);
 // A cancelled queue item still belongs to the old worker until its ACK.
 id=start();const auto first=id;cancel(id);uint32_t refused=99;
 assert(begin(source(),refused)==ESP_ERR_INVALID_STATE&&refused==0);
 uint32_t pending=0;assert(claimNext(pending,actual)&&pending==id&&actual==source());
 assert(!active(id)&&!write(id,"late"));complete(id,ESP_OK);
 id=start();assert(id!=first);cancel(first);complete(first,ESP_OK);
 assert(active(id));assert(claim(id,actual));assert(!claim(id,actual));
 // The final completion is visible only after every queued byte was drained.
 const std::string suffix(180,'x');assert(write(id,suffix));complete(id,ESP_OK);
 std::string received;Read result;
 do{result=read(id,out,sizeof(out),0);received.append(out,result.bytes);
    if(received.size()<suffix.size())assert(result.state==State::kStreaming);
 }while(result.state==State::kStreaming);
 assert(received==suffix&&result.state==State::kComplete);release(id);
 // No full answer fits the ring: the existing tasks must make progress together.
 id=start();assert(claim(id,actual));std::string payload;
 for(int i=0;i<20000;i++)payload+="é😀";
 std::atomic<bool> produced=false;
 std::thread producer([&]{assert(write(id,payload));complete(id,ESP_OK);produced=true;});
 received.clear();
 do{result=read(id,out,sizeof(out),100);received.append(out,result.bytes);
    assert(result.state==State::kStreaming||result.state==State::kComplete);
 }while(result.state!=State::kComplete);
 producer.join();assert(produced&&received==payload);release(id);
 // Disconnect while the worker is blocked on space; no subsequent query is hit.
 id=start();assert(claim(id,actual));std::atomic<bool> stopped=false;
 std::thread blocked([&]{assert(!write(id,payload));complete(id,ESP_OK);stopped=true;});
 result=read(id,out,1,100);assert(result.bytes==1);cancel(id);blocked.join();assert(stopped);
 const auto old=id;id=start();cancel(old);complete(old,ESP_FAIL);assert(active(id));
 // A provider failure after partial output cannot become a successful document.
 assert(claim(id,actual));assert(write(id,"{\"partial\":"));
 result=read(id,out,2,0);assert(result.bytes==2);complete(id,ESP_FAIL);
 result=read(id,out,sizeof(out),0);assert(!result.bytes&&result.state==State::kFailed&&result.error==ESP_FAIL);release(id);
 // The one existing absolute deadline includes queueing/backpressure.
 id=start();assert(deadlineMs(id)>0&&deadlineMs(id-1)==0);
 clock_offset+=int64_t(cfg::kOperationDeadlineMs)*1000;
 result=read(id,out,sizeof(out),0);assert(result.state==State::kFailed&&result.error==ESP_ERR_TIMEOUT);
 assert(claim(id,actual));assert(!write(id,"late"));complete(id,ESP_OK);
 assert(read(id,out,sizeof(out),0).error==ESP_ERR_TIMEOUT);release(id);
 // Empty successful answers, normal release and stale/invalid consumers.
 id=start();assert(claim(id,actual));complete(id,ESP_OK);
 result=read(id,out,sizeof(out),0);assert(!result.bytes&&result.state==State::kComplete);release(id);
 assert(read(id,out,sizeof(out),0).state==State::kStale);
 id=start();assert(read(id,nullptr,1,0).error==ESP_ERR_INVALID_ARG);
 cancel(id);assert(claim(id,actual));complete(id,ESP_OK);
 std::printf("original names: 8 lifecycle/backpressure scenarios PASS; pure host state=%zu B; no target runtime claim\n",sizeof(detail::Channel));
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix="football-original-channel-") as directory:
        scratch = Path(directory)
        (scratch / "freertos").mkdir()
        (scratch / "freertos/FreeRTOS.h").write_text(FREERTOS)
        (scratch / "freertos/event_groups.h").write_text(EVENTS)
        (scratch / "esp_err.h").write_text(
            "#pragma once\nusing esp_err_t=int;\n"
            "inline constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_ARG=0x102,"
            "ESP_ERR_INVALID_STATE=0x103,ESP_ERR_NO_MEM=0x101,ESP_ERR_TIMEOUT=0x107;\n")
        (scratch / "esp_timer.h").write_text("#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n")
        fixture = scratch / "channel.cpp"
        fixture.write_text(TEST)
        binary = scratch / "channel"
        includes = [scratch, ROOT / "include", ROOT / "components/football"]
        includes.extend((ROOT / "components").glob("*/include"))
        command = ["clang++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
                   "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread"]
        command.extend("-I" + str(path) for path in includes)
        command += [str(fixture), str(ROOT / "components/football/original_names_channel.cpp"),
                    "-o", str(binary)]
        subprocess.run(command, check=True)
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        subprocess.run([str(binary)], check=True, env=environment, timeout=20)

if __name__ == "__main__":
    main()
