#!/usr/bin/env python3
"""Whole files candidate/paths/I/O on real POSIX files with boundary faults."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="fc", dir="/tmp") as directory:
    folder = Path(directory)
    faults = folder / "faults.h"
    faults.write_text('''#pragma once
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
#define open(...) test_open(__VA_ARGS__)
#define read(...) test_read(__VA_ARGS__)
#define write(...) test_write(__VA_ARGS__)
#define close(...) test_close(__VA_ARGS__)
#define fsync(...) test_sync(__VA_ARGS__)
#define rename(...) test_rename(__VA_ARGS__)
#define unlink(...) test_unlink(__VA_ARGS__)
''')
    command = ["clang++", "-std=c++20", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-I" + str(ROOT / "include"),
               "-I" + str(ROOT / "components/files/include")]
    objects = []
    for name in ("file_candidate", "file_io"):
        target = folder / (name + ".o")
        subprocess.run(command + ["-include", str(faults), "-c",
                                   str(ROOT / "components/files" / (name + ".cpp")),
                                   "-o", str(target)], check=True)
        objects.append(str(target))
    binary = folder / "check"
    subprocess.run(command + [str(ROOT / "components/files/file_names.cpp"),
                              str(ROOT / "scripts/tests/fixtures/file_candidate_host.cpp"),
                              *objects, "-o", str(binary)], check=True)
    runtime = folder / "fs"
    runtime.mkdir()
    subprocess.run([str(binary), str(runtime)], check=True)
