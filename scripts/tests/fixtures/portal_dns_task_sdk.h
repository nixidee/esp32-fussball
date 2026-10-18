// Task allocation/ticks are SDK boundaries; the production loop stays intact.
#pragma once
#include <cstdint>
using StaticTask_t = int;
using StackType_t = uint32_t;
void* xTaskCreateStatic(void (*)(void*), const char*, uint32_t, void*, unsigned,
                        StackType_t*, StaticTask_t*);
void vTaskDelay(uint32_t ticks);
#define pdMS_TO_TICKS(ms) (ms)
