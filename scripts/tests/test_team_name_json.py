#!/usr/bin/env python3
"""Production streamed names importer + FNS2 + POSIX candidates, sanitizers."""
from pathlib import Path
import argparse
import json
import os
import random
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--export-dir", type=Path, help="retain actual v2 encoder output for browser replay")
args = parser.parse_args()
base = {"format": "football-team-names", "version": 1, "clubs": []}
cases = {}
def case(name, clubs, **extra):
    cases[name] = json.dumps(dict(base, clubs=clubs, **extra), ensure_ascii=False, separators=(",", ":"))

preset = json.loads((ROOT / "include/team_names.json").read_text())
random.Random(9).shuffle(preset["clubs"])
cases["good-preset"] = json.dumps(preset, ensure_ascii=False)
case("good-long-duplicate", [{"key":"a", "league":"x"*16260, "aliases":["x"*16260]*2}])
case("good-clipped", [{"short":"😀tail", "key":"a", "de":"a"*16384, "en":"ä😀"}])
case("good-empty", [{"de":"", "en":"", "short":"", "league":"", "key":"a"}])
case("good-dense-ids", [{"ids":{"espn":list(range(4000000000,4000005500))[::-1]}, "key":"a"}])
case("good-keys", [{"key":"k"*58+f"{n:05d}"} for n in range(1000)][::-1])
# The grouped corpus encodes to16089B; one293B pool entry plus its2B length
# fills the actual16384B wire ceiling, without changing its relation counts.
case("good-grouped", [{"key":f"club{n:04d}","aliases":["shared"]*6,
                       "ids":{"espn":[4000000000+n]},
                       **({"league":"z"*293} if n==0 else {})}
                      for n in range(400)][::-1])
case("good-exceptions", [{"key":"z", "ids":{"espn":[42, 5]}, "aliases":["same","same"]},
                         {"key":"a", "ids":{"openligadb":[42]}}],
     exceptions=[{"name":"😀", "id":4294967295, "provider":3}, {"id":2, "provider":0, "name":""}])
case("good-v2-long-duplicate", [{"key":"a", "league":16384, "aliases":[16384,16384]}],
     version=2, texts={"16384":"x"*16260})
case("good-v2-many-aliases", [{"key":"a", "league":9, "aliases":[9]*2000}],
     version=2, texts={"9":"x"*8136})
case("good-v2-empty", [{"de":7, "en":7, "short":7, "league":7, "key":"a"}],
     version=2, texts={"7":""})
case("good-v2-dictionary-first", [{"key":"a", "de":2, "short":1, "ids":{"espn":[42]}}],
     version=2, texts={"2":"Name", "1":"FCB"}, exceptions=[{"id":42,"provider":2,"name":3}])
obj = json.loads(cases["good-v2-dictionary-first"])
obj["texts"]["3"] = ""
cases["good-v2-dictionary-first"] = json.dumps(dict(texts=obj.pop("texts"), **obj), separators=(",",":"))
case("bad-v2-missing-text", [{"key":"a", "de":2}], version=2, texts={"1":"x"})
case("bad-v2-unreferenced", [{"key":"a"}], version=2, texts={"1":"x"})
case("bad-v2-duplicate-text", [{"key":"a", "de":1, "en":2}], version=2, texts={"1":"x","2":"x"})
case("bad-v2-long-adopted", [{"key":"a", "de":1}], version=2, texts={"1":"x"*27})
case("bad-v2-inline", [{"key":"a", "de":"x"}], version=2, texts={})
case("bad-v1-reference", [{"key":"a", "de":1}])
case("bad-v2-text-zero", [{"key":"a", "de":0}], version=2, texts={"0":"x"})
case("bad-v2-text-leading-zero", [{"key":"a", "de":1}], version=2, texts={"01":"x"})
case("bad-overflow", [{"key":"a", "league":"x"*16384}])
case("bad-id-zero", [{"key":"a", "ids":{"espn":[0]}}])
case("bad-id-duplicate", [{"key":"a", "ids":{"espn":[5]}},{"key":"b", "ids":{"espn":[5]}}])
case("bad-key-duplicate", [{"key":"a"},{"key":"a"}])
case("bad-exception-duplicate", [], exceptions=[{"provider":0,"id":1,"name":"a"},{"provider":0,"id":1,"name":"b"}])
case("bad-nul-tail", [{"key":"a", "de":"a"*16384+"\0"}])
case("bad-key-long", [{"key":"x"*64}])
case("bad-type", [{"key":"a", "de":None}])
case("bad-unknown", [{"key":"a", "league2":"x"}])
cases["bad-string-tail"] = cases["good-clipped"].replace('"en":"ä😀"', '"en":"'+"a"*16384+'\\uD800"')
cases["bad-key-escaped-duplicate"] = '{"format":"football-team-names","version":1,"clubs":[{"key":"a","\\u006bey":"b"}]}'
cases["bad-after-complete"] = cases["good-empty"]+" false"

with tempfile.TemporaryDirectory(prefix="nj", dir="/tmp") as directory:
    work = Path(directory)
    # Runtime POSIX root must fit the same production path buffer as /fs.
    fs = work / "fs"
    fs.mkdir()
    (work / "esp_err.h").write_text("#pragma once\nusing esp_err_t=int;\n")
    # Whole names/files owners compile unchanged. Adapt only POSIX calls in
    # common I/O/candidate objects, retaining the real36B path bound.
    (work / "faults.h").write_text('''#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstddef>
ssize_t names_test_read(int,void*,std::size_t);
ssize_t names_test_write(int,const void*,std::size_t);
int names_test_sync(int);int names_test_close(int);
int names_test_open(const char*,int,int=0);
#define open(...) names_test_open(__VA_ARGS__)
#define read(...) names_test_read(__VA_ARGS__)
#define write(...) names_test_write(__VA_ARGS__)
#define close(...) names_test_close(__VA_ARGS__)
#define fsync(...) names_test_sync(__VA_ARGS__)
''')
    paths = []
    for name, text in cases.items():
        path = work / (name+".json")
        path.write_text(text)
        paths.append(str(path))
    fixture = (ROOT / "scripts/tests/fixtures/team_name_json_host.cpp").read_text()
    service = (ROOT / "components/names/team_name_service.cpp").read_text()
    start = service.index("namespace {\nbool storedString")
    end = service.index("esp_err_t readStatus()", start)
    fixture = fixture.replace("// ACTUAL_EXPORT", service[start:end])
    (work / "main.cpp").write_text(fixture)
    sources = [ROOT / "components/names/team_name_json.cpp",
               ROOT / "components/names/team_name_store.cpp", ROOT / "components/names/team_name_files.cpp",
               ROOT / "components/football/football_model.cpp",
               ROOT / "components/timekeeping/civil_time.cpp",
               ROOT / "components/files/file_names.cpp",
               ROOT / "components/json/json_stream.cpp",
               work / "main.cpp"]
    command = ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DFUSSBALL_NATIVE"]
    for path in [work, ROOT / "include", *(ROOT / "components").glob("*/include")]:
        command += ["-I", str(path)]
    for name in ("file_io", "file_candidate"):
        target = work / (name + ".o")
        subprocess.run(command + ["-include", str(work / "faults.h"), "-c",
                                   str(ROOT / "components/files" / (name + ".cpp")),
                                   "-o", str(target)], check=True)
        sources.append(target)
    command += list(map(str, sources)) + ["-o", str(work / "check")]
    subprocess.run(command, check=True)
    environment = dict(os.environ)
    if args.export_dir:
        args.export_dir.mkdir(parents=True, exist_ok=True)
        environment["NAMES_EXPORT_DIR"] = str(args.export_dir.resolve())
    subprocess.run([str(work / "check"), str(fs), *paths], check=True, env=environment)
