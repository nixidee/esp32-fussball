#!/usr/bin/env python3
"""Exercise real scene admission/last-SPI-stripe ordering, without a renderer."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = r'''
#include <cassert>
#include <cstdio>
#include "scene_progress.h"
using display::detail::SceneProgress;
int main() {
 SceneProgress p;
 assert(p.accept(0)); assert(!p.accept(0));
 assert(p.begin(false)); p.submitted(); p.completed(10);
 assert(!p.completion().valid); // first stripe cannot start a visit
 assert(p.begin(true)); p.completed(20);
 assert(!p.completion().valid); // ISR may precede the driver's return
 p.submitted(); assert(p.completion().valid && p.completion().token==0);
 assert(p.completion().at_ms==20);
 assert(p.accept(1)); assert(p.begin(false)); p.submitted();
 assert(p.begin(true)); p.submitted(); p.completed(30);
 assert(p.completion().token==0); p.completed(40);
 assert(p.completion().token==1 && p.completion().at_ms==40);
 assert(p.accept(2)); assert(p.begin(false)); p.submitted(); p.completed(50);
 assert(p.begin(false)); p.failed();
 assert(p.begin(true)); p.submitted(); p.completed(60);
 assert(p.completion().token==1); // any failed stripe rejects the whole pass
 assert(p.begin(false)); p.submitted(); p.completed(70);
 assert(p.begin(true)); p.submitted(); p.completed(80);
 assert(p.completion().token==2 && p.completion().at_ms==80); // redraw recovers
 assert(p.accept(3)); assert(p.begin(true)); p.completed(90); p.failed();
 assert(p.completion().token==2); // drain after uncertain submission cannot ACK
 assert(p.begin(true)); p.submitted();
 assert(p.accept(4)); p.completed(100);
 assert(p.completion().token==2); // old final stripe cannot ACK the newer scene
 assert(p.begin(true)); p.submitted(); p.completed(110);
 assert(p.completion().token==4 && p.completion().at_ms==110);
 assert(p.accept(5)); assert(p.begin(false)); p.submitted();
 assert(p.begin(true)); p.submitted(); assert(!p.begin(true));
 p.completed(120); p.completed(130); assert(p.completion().token==5);
 std::printf("scene progress: 8 lifecycle/failure scenarios, %zu B fixed state\n", sizeof(p));
}
'''
with tempfile.TemporaryDirectory(prefix="football-scene-") as directory:
    scratch = Path(directory)
    (scratch / "esp_err.h").write_text("#pragma once\nusing esp_err_t = int;\n")
    cpp = scratch / "scene.cpp"
    cpp.write_text(source)
    binary = scratch / "scene"
    subprocess.run(["clang++", "-std=c++20", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I", str(scratch),
                    "-I", str(root / "include"), "-I", str(root / "components/display"),
                    "-I", str(root / "components/display/include"), str(cpp),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
