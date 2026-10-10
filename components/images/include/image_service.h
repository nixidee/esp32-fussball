#pragma once
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
namespace images {
struct Entry {
  const char* name;
  std::size_t bytes;
  bool uploaded, available;
};
Entry info(uint8_t index);
bool source(uint8_t index, char* out, std::size_t capacity);
esp_err_t begin(uint8_t index, std::size_t size);
esp_err_t write(const uint8_t* bytes, std::size_t size);
esp_err_t finish(int64_t deadline_ms);
void abort();
esp_err_t reset(uint8_t index);
esp_err_t resetAll();
// Explicit full storage reset, including recovery from damaged LittleFS.
// Caller owns the heavy-operation admission; unavailable during OTA trial.
esp_err_t format();
uint32_t revision();
}  // namespace images
