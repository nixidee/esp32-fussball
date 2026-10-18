#pragma once
#include "esp_err.h"
namespace web {
esp_err_t init();
// Main-loop retry of failed startup; a completed server/debug worker is reused.
void poll();
bool healthy();
}  // namespace web
