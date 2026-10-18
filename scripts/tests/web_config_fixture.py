#!/usr/bin/env python3
"""Emit the current production schema/config codecs for controlled client tests."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
web = (ROOT / "components/web/web_service.cpp").read_text()
demo_handler = web[web.index("esp_err_t getDemo("):web.index("esp_err_t postDemo(")]
demo_source = (ROOT / "components/football/demo_scenarios.cpp").read_text()
demo_metadata = demo_source[demo_source.index("const char* key("):demo_source.index("Bindings configure(")]
schema = web[web.index("settings::screen_policy::Limits screenLimits()"):web.index("esp_err_t getStatus(")]
prelude = r'''
#include <cassert>
#include <iostream>
#include <string>
#include "config_json.h"
#include "football_model.h"
#include "football_service.h"
#include "screen_json.h"
#include "team_name_store.h"
#include "targets/xiao_esp32c6_gc9a01.h"
using esp_err_t=int;
namespace ota {uint32_t capacity(){return 0x1c0000;}}
namespace football {DemoStatus demoStatus(){return {};}}
namespace settings {
const Limits& limits(){static const Limits value{hw::kDisplay.visible_matches_max,hw::kDisplay.table_window_max,static_cast<uint8_t>(hw::kTarget.inputs.size())};return value;}
}
struct httpd_req_t {std::string body;};
struct Request {explicit Request(httpd_req_t*){}};
template<class F>int streamJson(httpd_req_t*req,F fields){
 json::Writer writer({req,[](std::string_view text,void*p){static_cast<httpd_req_t*>(p)->body.append(text);return true;}});
 assert(writer.object()&&fields(writer)&&writer.end()&&writer.finish());return 0;
}
using namespace web;
constexpr auto kShortNameBytes=cfg::kTeamShortBytes;
'''
main = r'''
int main(){
 httpd_req_t schema;getSchema(&schema);
 httpd_req_t demo;getDemo(&demo);
 settings::AppConfig config;config.model=settings::initialValues({},nullptr,settings::limits());config.revision=12;config.last_apply_id=1;
 // Assemble the synthetic populated fixture with the same target clipping as
 // initialConfig(); no presets, NVS or user secrets are read by this program.
 config.screens.visible_matches=std::min(config.screens.visible_matches,settings::limits().visible_matches_max);
 config.screens.table_window=std::min(config.screens.table_window,settings::limits().table_window_max);
 settings::Competition entry;entry.id=23;entry.incarnation=800;entry.primary.competition.assign("bl1");entry.primary.season.assign("2026");entry.competition_name.assign("Bundesliga");entry.club.team=65;entry.club.name.assign("FC");config.competitions.entries.push(entry);
 std::string body;json::Writer out({&body,[](std::string_view text,void*p){static_cast<std::string*>(p)->append(text);return true;}});
 assert(out.object()&&configJson(out,config)&&out.end()&&out.finish());
 std::cout<<"{\"schema\":"<<schema.body<<",\"config\":"<<body<<",\"demo\":"<<demo.body<<"}\n";
}
'''
with tempfile.TemporaryDirectory(prefix="football-web-codec-fixture-") as directory:
    scratch = Path(directory)
    source = scratch / "fixture.cpp"
    (scratch / "esp_err.h").write_text("#pragma once\nusing esp_err_t=int;\n")
    source.write_text(prelude + schema + "namespace football::demo {\n" + demo_metadata + "\n}\n" + demo_handler + main)
    command = ["clang++", "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror"]
    command += ["-I", str(scratch), "-iquote", str(ROOT)]
    command += ["-I", str(ROOT / ".pio/build/xiao_esp32c6_gc9a01/config")]
    for path in [ROOT / "include", ROOT / "components/web"]:
        command += ["-I", str(path)]
    for path in (ROOT / "components").glob("*/include"):
        command += ["-I", str(path)]
    sources = [source]
    sources.extend(ROOT / name for name in [
        "components/json/json_stream.cpp", "components/settings/settings_model.cpp",
        "components/settings/screen_policy.cpp", "components/timekeeping/civil_time.cpp",
        "components/football/football_model.cpp", "components/web/settings_json.cpp",
        "components/web/screen_json.cpp", "components/web/config_json.cpp"])
    command += [*map(str, sources), "-o", str(scratch / "fixture")]
    subprocess.run(command, check=True)
    subprocess.run([str(scratch / "fixture")], check=True)
