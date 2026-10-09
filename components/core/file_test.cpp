// Device tests of the file service, selected by cfg::kFileTest
// (app_config.h). Expected results: docs/ARCHITECTURE.md, "File service".
// Each test spans two boots: the first prepares the storage, the second
// (press RST) checks how init() treated it.

#include <array>
#include <cerrno>
#include <cstring>

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fcntl.h"
#include "file_service.h"
#include "file_service_internal.h"
#include "sys/stat.h"
#include "unistd.h"

namespace files {

namespace {

constexpr const char* kTag = "files_test";

using cfg::FileTest;

// kCorrupt overwrites both LittleFS superblock copies (blocks 0 and 1).
constexpr std::size_t kCorruptBlocks = 2;
constexpr uint8_t kCorruptPattern = 0x5A;
constexpr std::size_t kChunkBytes = 256;
// kFormat writes this many chunks (4 KB) with a position-dependent pattern.
constexpr std::size_t kTestFileChunks = 16;
constexpr char kTestFileName[] = "format_test.bin";

std::size_t freeHeap() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void fillChunk(std::array<uint8_t, kChunkBytes>& chunk, std::size_t index) {
  for (std::size_t i = 0; i < chunk.size(); ++i) {
    chunk[i] = static_cast<uint8_t>(index * 7 + i);
  }
}

// True if the corrupt pattern is still in both blocks.
bool corruptPatternKept(const esp_partition_t* partition) {
  std::array<uint8_t, kChunkBytes> chunk;
  const std::size_t bytes = kCorruptBlocks * partition->erase_size;
  for (std::size_t offset = 0; offset < bytes; offset += chunk.size()) {
    if (esp_partition_read(partition, offset, chunk.data(), chunk.size()) !=
        ESP_OK) {
      return false;
    }
    for (uint8_t b : chunk) {
      if (b != kCorruptPattern) return false;
    }
  }
  return true;
}

void runCorrupt(const esp_partition_t* partition) {
  if (state() == State::kUnavailable) {
    const bool kept = corruptPatternKept(partition);
    const bool pass = kept && !initialisedAtBoot();
    ESP_LOGI(kTag, "%s: damaged filesystem %s, initialised at boot: %s",
             pass ? "PASS" : "FAIL", kept ? "kept" : "CHANGED",
             initialisedAtBoot() ? "YES" : "no");
    return;
  }
  if (state() != State::kMounted) {
    ESP_LOGE(kTag, "FAIL: unexpected state %s", stateName(state()));
    return;
  }
  esp_err_t err = internal::unmount();
  const std::size_t bytes = kCorruptBlocks * partition->erase_size;
  if (err == ESP_OK) err = esp_partition_erase_range(partition, 0, bytes);
  std::array<uint8_t, kChunkBytes> chunk;
  chunk.fill(kCorruptPattern);
  for (std::size_t offset = 0; err == ESP_OK && offset < bytes;
       offset += chunk.size()) {
    err = esp_partition_write(partition, offset, chunk.data(), chunk.size());
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "FAIL: could not damage the filesystem: %s",
             esp_err_to_name(err));
    return;
  }
  ESP_LOGW(kTag, "superblocks overwritten; press RST to check the next boot");
}

void runErase(const esp_partition_t* partition) {
  if (initialisedAtBoot()) {
    Usage use{};
    const bool pass = state() == State::kMounted && usage(use) == ESP_OK;
    ESP_LOGI(kTag, "%s: erased partition initialised, %u of %u B used",
             pass ? "PASS" : "FAIL", static_cast<unsigned>(use.used_bytes),
             static_cast<unsigned>(use.total_bytes));
    return;
  }
  const int64_t start_us = esp_timer_get_time();
  esp_err_t err = internal::unmount();
  if (err == ESP_OK) {
    err = esp_partition_erase_range(partition, 0, partition->size);
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "FAIL: erase: %s", esp_err_to_name(err));
    return;
  }
  ESP_LOGW(kTag, "partition erased in %u ms; press RST to check the next boot",
           static_cast<unsigned>((esp_timer_get_time() - start_us) / 1000));
}

esp_err_t writeTestFile(const char* path) {
  const std::size_t heap_before = freeHeap();
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return ESP_FAIL;
  ESP_LOGI(kTag, "open file for writing costs %u B heap",
           static_cast<unsigned>(heap_before - freeHeap()));
  std::array<uint8_t, kChunkBytes> chunk;
  esp_err_t err = ESP_OK;
  for (std::size_t i = 0; err == ESP_OK && i < kTestFileChunks; ++i) {
    fillChunk(chunk, i);
    if (write(fd, chunk.data(), chunk.size()) !=
        static_cast<ssize_t>(chunk.size())) {
      err = ESP_FAIL;
    }
  }
  if (close(fd) != 0) err = ESP_FAIL;
  return err;
}

bool testFileMatches(const char* path) {
  const int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  std::array<uint8_t, kChunkBytes> expected;
  std::array<uint8_t, kChunkBytes> actual;
  bool match = true;
  for (std::size_t i = 0; match && i < kTestFileChunks; ++i) {
    fillChunk(expected, i);
    match = read(fd, actual.data(), actual.size()) ==
                static_cast<ssize_t>(actual.size()) &&
            expected == actual;
  }
  // Nothing after the written data.
  uint8_t extra = 0;
  if (match) match = read(fd, &extra, 1) == 0;
  close(fd);
  return match;
}

void runFormat() {
  if (state() != State::kMounted) {
    ESP_LOGE(kTag, "FAIL: needs a mounted filesystem, state %s",
             stateName(state()));
    return;
  }
  std::array<char, kPathBufferBytes> path;
  if (!buildPath(cfg::kFsBasePath, kTestFileName, path)) {
    ESP_LOGE(kTag, "FAIL: test file name rejected");
    return;
  }
  struct stat info{};
  if (stat(path.data(), &info) != 0) {
    // First boot: create the file; the next boot reads it after a restart.
    if (writeTestFile(path.data()) != ESP_OK || !testFileMatches(path.data())) {
      ESP_LOGE(kTag, "FAIL: test file not written (errno %d)", errno);
      return;
    }
    ESP_LOGW(kTag, "%s written (%u B); press RST to check the next boot",
             kTestFileName,
             static_cast<unsigned>(kTestFileChunks * kChunkBytes));
    return;
  }
  const bool survived = testFileMatches(path.data());
  const esp_err_t err = format();
  const bool gone = stat(path.data(), &info) != 0 && errno == ENOENT;
  Usage use{};
  const bool mounted = state() == State::kMounted && usage(use) == ESP_OK;
  const bool pass = survived && err == ESP_OK && gone && mounted;
  ESP_LOGI(kTag,
           "%s: file after restart %s; format %s; file %s; %u of %u B used",
           pass ? "PASS" : "FAIL", survived ? "intact" : "DAMAGED",
           esp_err_to_name(err), gone ? "gone" : "STILL THERE",
           static_cast<unsigned>(use.used_bytes),
           static_cast<unsigned>(use.total_bytes));
}

}  // namespace

void runDeviceTest() {
  if constexpr (cfg::kFileTest != FileTest::kNone) {
    ESP_LOGW(kTag, "device test %u active (app_config.h kFileTest)",
             static_cast<unsigned>(cfg::kFileTest));
    const esp_partition_t* partition = internal::partition();
    if (partition == nullptr) {
      ESP_LOGE(kTag, "FAIL: no littlefs partition");
      return;
    }
    switch (cfg::kFileTest) {
      case FileTest::kCorrupt: runCorrupt(partition); break;
      case FileTest::kErase: runErase(partition); break;
      case FileTest::kFormat: runFormat(); break;
      case FileTest::kNone: break;
    }
  }
}

}  // namespace files
