// Application-owned original-name export; HTTP consumes the bounded channel.
#pragma once
#include <cstdint>

#include "esp_err.h"
namespace application {
esp_err_t beginOriginalNames(uint8_t competition, uint32_t& query);
}
