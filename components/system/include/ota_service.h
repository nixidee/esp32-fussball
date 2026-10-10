#pragma once
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
namespace ota {
struct Identity {
  uint32_t magic, version;
  char target[48], layout[32];
  uint32_t settings_format;
};
void init(bool local_health);
void startupHealth(bool local_health);
void poll(bool local_health);
bool trial();
void reboot();
esp_err_t begin(const uint8_t* prefix, std::size_t prefix_size,
                std::size_t total);
esp_err_t write(const uint8_t* bytes, std::size_t count);
esp_err_t finish();
void abort();
const Identity& identity();
std::size_t capacity();
}  // namespace ota
