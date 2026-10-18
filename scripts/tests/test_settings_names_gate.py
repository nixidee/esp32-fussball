"""Check the actual settings names-publication gate under a semaphore adapter.

Extract only generation() and the new gate functions. Never compile the complete
settings store or include/expand personal presets. The competing writer models
the existing settings save's verified write_mutex critical-section boundary.
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


PRELUDE = r'''
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using esp_err_t=int;
constexpr esp_err_t ESP_OK=0,ESP_ERR_TIMEOUT=1,ESP_ERR_INVALID_STATE=2;
constexpr unsigned pdTRUE=1,portMAX_DELAY=UINT32_MAX;
struct Semaphore {
 std::mutex guard;
 std::condition_variable changed;
 bool held=false;
 unsigned takes=0,gives=0,waiters=0,last_wait=0;
};
Semaphore shared_write_mutex;
Semaphore* const write_mutex=&shared_write_mutex;
std::atomic<uint32_t> generation_counter{13};
bool nvs_ready=true,record_unread=false;
std::atomic<unsigned> writes{0};
unsigned xSemaphoreTake(Semaphore* semaphore,unsigned wait){
 std::unique_lock lock(semaphore->guard);
 ++semaphore->takes;semaphore->last_wait=wait;
 if(wait==0&&semaphore->held)return 0;
 if(semaphore->held){
  ++semaphore->waiters;semaphore->changed.notify_all();
  semaphore->changed.wait(lock,[&]{return !semaphore->held;});
  --semaphore->waiters;
 }
 semaphore->held=true;return pdTRUE;
}
void xSemaphoreGive(Semaphore* semaphore){
 std::lock_guard lock(semaphore->guard);
 if(!semaphore->held)throw std::runtime_error("unowned semaphore release");
 semaphore->held=false;++semaphore->gives;semaphore->changed.notify_all();
}
void check(bool valid,const char* message){if(!valid)throw std::runtime_error(message);}
'''

MAIN = r'''
void prepare(uint32_t value=13){
 std::lock_guard lock(shared_write_mutex.guard);
 check(!shared_write_mutex.held&&shared_write_mutex.waiters==0,"previous gate released");
 shared_write_mutex.takes=shared_write_mutex.gives=shared_write_mutex.last_wait=0;
 generation_counter.store(value,std::memory_order_release);writes.store(0);
}
void competingSettingsSave(){
 // This boundary is checked against the actual settings::save source below.
 xSemaphoreTake(write_mutex,portMAX_DELAY);
 writes.fetch_add(1,std::memory_order_release);
 generation_counter.fetch_add(1,std::memory_order_release);
 xSemaphoreGive(write_mutex);
}
int main(){try{
 prepare();check(xSemaphoreTake(write_mutex,0)==pdTRUE,"controlled existing writer");
 check(beginNamesPublication(13)==ESP_ERR_TIMEOUT,"busy gate refuses immediately");
 check(shared_write_mutex.last_wait==0&&shared_write_mutex.gives==0&&writes.load()==0&&generation()==13,"busy refusal performs no release/write/generation mutation");
 xSemaphoreGive(write_mutex);

 prepare();check(beginNamesPublication(12)==ESP_ERR_INVALID_STATE,"stale generation refused");
 check(!shared_write_mutex.held&&shared_write_mutex.gives==1&&generation()==13&&writes.load()==0,"stale refusal releases only acquired mutex and changes no settings");

 prepare();check(beginNamesPublication(13)==ESP_OK&&shared_write_mutex.held,"matching generation holds existing writer mutex");
 check(shared_write_mutex.gives==0&&generation()==13,"successful begin does not release or publish a generation");
 check(beginNamesPublication(13)==ESP_ERR_TIMEOUT,"a second publication cannot enter held gate");
 check(shared_write_mutex.gives==0,"nested refusal does not release first publication");
 endNamesPublication();check(!shared_write_mutex.held&&shared_write_mutex.gives==1,"explicit end releases successful gate");

 prepare();check(beginNamesPublication(13)==ESP_OK,"publication fence before competing save");
 std::thread writer(competingSettingsSave);
 {
  std::unique_lock lock(shared_write_mutex.guard);
  check(shared_write_mutex.changed.wait_for(lock,std::chrono::seconds(2),[]{return shared_write_mutex.waiters==1;}),"competing settings writer reaches same mutex");
  check(writes.load()==0&&generation()==13,"settings save cannot pass while names publication is held");
 }
 endNamesPublication();writer.join();
 check(writes.load()==1&&generation()==14&&!shared_write_mutex.held,"settings writer proceeds only after coherent publication releases existing lock");

 prepare(UINT32_MAX);check(beginNamesPublication(UINT32_MAX)==ESP_OK,"maximum uint32 generation is valid");endNamesPublication();
 generation_counter.store(0,std::memory_order_release);
 check(beginNamesPublication(UINT32_MAX)==ESP_ERR_INVALID_STATE&&!shared_write_mutex.held,"wrapped generation rejects stale pre-wrap snapshot");
 check(beginNamesPublication(0)==ESP_OK,"zero generation is valid after wrap");endNamesPublication();
 std::cout<<"6 settings names-gate scenarios PASS; actual extracted gate/getter; controlled semaphore and competing writer boundary\n";
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
'''


def function(source, signature):
    start = source.index(signature)
    if signature.startswith("void endNamesPublication"):
        return source[start:source.index("\n", start)]
    return source[start:source.index("\n}", start) + 2]


def main():
    root = Path(__file__).resolve().parents[2]
    # Reading this source does not read or expand its include directives.
    source = (root / "components/settings/settings_store.cpp").read_text()
    save = function(source, "esp_err_t save(const Model&")
    if "xSemaphoreTake(write_mutex, portMAX_DELAY);" not in save:
        raise SystemExit("Settings save no longer uses the verified writer mutex boundary")
    bodies = "\n".join(function(source, signature) for signature in (
        "uint32_t generation()", "esp_err_t beginNamesPublication(",
        "void endNamesPublication()",
    ))
    compiler = shutil.which("clang++")
    if compiler is None:
        raise SystemExit("clang++ is required for the publication gate check")
    with tempfile.TemporaryDirectory(prefix="names-settings-gate-") as temporary:
        folder = Path(temporary)
        source_file = folder / "gate.cpp"
        source_file.write_text(PRELUDE + bodies + MAIN)
        binary = folder / "check"
        subprocess.run([
            compiler, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
            str(source_file), "-o", str(binary),
        ], cwd=root, check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = (
            "detect_leaks=" + ("0" if sys.platform == "darwin" else "1")
            + ":abort_on_error=1"
        )
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        subprocess.run([str(binary)], cwd=root, env=environment, check=True)


if __name__ == "__main__":
    main()
