#!/usr/bin/env python3
"""Actual request-budget NVS adapter under controlled reads/writes, not a device.

Current counters and admission are production code. Only SDK calls are replaced;
each replay starts a fresh process with the real static counter owner.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

SDK = r'''
#pragma once
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NVS_NOT_FOUND = 1,
              ESP_ERR_INVALID_ARG = 2, ESP_ERR_NOT_ALLOWED = 3;
using nvs_handle_t = int;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
inline std::map<std::string, std::vector<unsigned char>> disk;
inline std::vector<std::string> reads, writes;
inline std::string fail_read;
inline bool fail_set = false, fail_commit = false;
inline unsigned erases = 0;
inline int nvs_open(const char*, int, nvs_handle_t* handle) {
  *handle = 1; return ESP_OK;
}
inline void nvs_close(nvs_handle_t) {}
inline int nvs_get_blob(nvs_handle_t, const char* key, void* out, size_t* size) {
  reads.emplace_back(key);
  if (fail_read == key) return ESP_FAIL;
  const auto found = disk.find(key);
  if (found == disk.end()) return ESP_ERR_NVS_NOT_FOUND;
  if (*size < found->second.size()) return ESP_FAIL;
  *size = found->second.size();
  std::memcpy(out, found->second.data(), *size);
  return ESP_OK;
}
inline int nvs_set_blob(nvs_handle_t, const char* key, const void* bytes, size_t size) {
  writes.emplace_back(key);
  if (fail_set) return ESP_FAIL;
  const auto* begin = static_cast<const unsigned char*>(bytes);
  disk[key] = {begin, begin + size};
  return ESP_OK;
}
inline int nvs_commit(nvs_handle_t) { return fail_commit ? ESP_FAIL : ESP_OK; }
inline int nvs_erase_all(nvs_handle_t) { ++erases; disk.clear(); return ESP_OK; }
template<class... T> inline void logTest(const T&...) {}
#define ESP_LOGE(...) logTest(__VA_ARGS__)
'''

MAIN = r'''
#include "request_budget.h"
#include "budget_ledger.h"
#include "nvs.h"
#include <algorithm>
#include <iostream>
namespace budget = football::budget;
constexpr auto provider = cfg::Provider::kApiFootball;
constexpr uint32_t day = 20000;
void put(const char* key, uint32_t reserved) {
  const budget::Record record{cfg::kBudgetRecordMagic, day, reserved};
  const auto* bytes = reinterpret_cast<const unsigned char*>(&record);
  disk[key] = {bytes, bytes + sizeof(record)};
}
void checkedBoot() {
  assert(budget::init() == ESP_OK);
  assert(reads.size() == cfg::kProviderCount + 1);
  assert(std::find(reads.begin(), reads.end(), "credits") == reads.end());
  assert(erases == 0);
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string test = argv[1];
  if (test == "current") {
    put("api_football", 42); put("global", 9);
    disk["credits"] = std::vector<unsigned char>(24, 0xa5);
    const auto obsolete = disk["credits"];
    checkedBoot();
    assert(budget::remaining(provider, day, 45) == 3);
    assert(budget::remainingGlobal(day, 10) == 1);
    for (unsigned i = 0; i < 3; ++i)
      assert(budget::admit(provider, day, 45) == ESP_OK);
    assert(budget::admit(provider, day, 45) == ESP_ERR_NOT_ALLOWED);
    assert(budget::admitGlobal(day, 10) == ESP_OK);
    assert(budget::admitGlobal(day, 10) == ESP_ERR_NOT_ALLOWED);
    assert(disk["credits"] == obsolete && writes.size() == 2);
  } else if (test == "no-migration") {
    disk["credits"] = std::vector<unsigned char>(24, 0xa5);
    checkedBoot();
    assert(budget::remaining(provider, day, 100) == 100);
    assert(budget::admit(provider, day, 100) == ESP_OK);
    assert(budget::remaining(provider, day, 100) == 99);
    assert(writes == std::vector<std::string>{"api_football"});
    assert(disk["api_football"].size() == 12 && disk["credits"].size() == 24);
  } else if (test == "set-commit-failure") {
    put("api_football", 8); checkedBoot();
    fail_set = true;
    assert(budget::admit(provider, day, 10) == ESP_FAIL);
    assert(budget::remaining(provider, day, 10) == 2);
    fail_set = false; fail_commit = true;
    assert(budget::admit(provider, day, 10) == ESP_FAIL);
    assert(budget::remaining(provider, day, 10) == 2);
    fail_commit = false;
    assert(budget::admit(provider, day, 10) == ESP_OK);
    assert(budget::remaining(provider, day, 10) == 1);
    const auto before = writes.size();
    assert(budget::admit(provider, day, 10) == ESP_OK);
    assert(writes.size() == before);
    assert(budget::admit(provider, day, 10) == ESP_ERR_NOT_ALLOWED);
  } else if (test == "damaged" || test == "read-failure") {
    put("api_football", 8); put("global", 3);
    if (test == "damaged") disk["api_football"][0] ^= 1;
    else fail_read = "api_football";
    checkedBoot(); budget::settle(day);
    assert(budget::remaining(provider, day, 100) == 0);
    assert(budget::admit(provider, day, 100) == ESP_ERR_NOT_ALLOWED);
    assert(budget::remainingGlobal(day, 20) == 17);
    assert(budget::admit(cfg::Provider::kOpenLigaDb, day, 100) == ESP_OK);
    assert(budget::remaining(provider, day + 1, 100) == 100);
    assert(budget::admit(provider, day - 1, 100) == ESP_ERR_NOT_ALLOWED);
    assert(budget::admit(provider, day + 1, 100) == ESP_OK);
    assert(budget::remaining(provider, day + 1, 100) == 99);
  } else if (test == "disabled-invalid") {
    checkedBoot();
    assert(budget::admit(provider, day, 0) == ESP_OK);
    assert(budget::admitGlobal(day, 0) == ESP_OK);
    assert(budget::admit(static_cast<cfg::Provider>(cfg::kProviderCount), day, 10)
           == ESP_ERR_INVALID_ARG);
    assert(writes.empty() && disk.empty());
  } else if (test == "explicit-reset") {
    put("api_football", 8); put("global", 3);
    disk["credits"] = std::vector<unsigned char>(24, 0xa5);
    checkedBoot(); assert(budget::erase() == ESP_OK);
    assert(disk.empty() && erases == 1);
  } else assert(false);
  std::cout << test << " PASS\n";
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="request-budget-") as folder:
        work = Path(folder)
        for name in ("esp_err.h", "nvs.h", "esp_log.h"):
            (work / name).write_text('#include "sdk_test.h"\n')
        (work / "sdk_test.h").write_text(SDK)
        (work / "main.cpp").write_text(MAIN)
        binary = work / "replay"
        subprocess.run([
            os.environ.get("CXX", "clang++"), "-std=c++20", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(work), "-I", str(ROOT / "include"),
            "-I", str(ROOT / "components/football/include"),
            str(ROOT / "components/football/request_budget.cpp"),
            str(ROOT / "components/football/budget_ledger.cpp"),
            str(work / "main.cpp"), "-o", str(binary),
        ], check=True)
        for case in ("current", "no-migration", "set-commit-failure", "damaged",
                     "read-failure", "disabled-invalid", "explicit-reset"):
            subprocess.run([str(binary), case], check=True)


if __name__ == "__main__":
    main()
