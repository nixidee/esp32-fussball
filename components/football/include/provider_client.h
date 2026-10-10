#pragma once
#include <ArduinoJson.h>

#include <cstddef>

#include "esp_err.h"
#include "football_model.h"
#include "settings_model.h"
namespace football {
// One capped document at a time, released after an array element is mapped.
class JsonAllocator : public ArduinoJson::Allocator {
 public:
  void* allocate(std::size_t size) override;
  void deallocate(void* pointer) override;
  void* reallocate(void* pointer, std::size_t size) override;
  std::size_t used() const { return used_; }

 private:
  std::size_t used_ = 0;
};
using JsonConsumer = bool (*)(ArduinoJson::JsonVariantConst item,
                              void* context);
struct FixtureFilter {
  const char* team = "";
  uint16_t round = 0;
};
// HTTP status (0 = none received) and body bytes read, for logging only.
struct FetchResult {
  int status = 0;
  std::size_t bytes = 0;
};
esp_err_t fetchJson(const char* url, const char* array_key,
                    cfg::Provider provider, const settings::Model& model,
                    uint32_t generation, JsonConsumer consumer, void* context,
                    bool selection = false, FetchResult* result = nullptr);
bool mapOpenLigaMatch(ArduinoJson::JsonVariantConst row, Snapshot& out,
                      uint8_t route, int64_t now, FixtureFilter filter = {});
bool mapMatch(ArduinoJson::JsonVariantConst row, Snapshot& out,
              cfg::Provider provider, uint8_t route, int64_t now,
              FixtureFilter filter = {});
bool mapTable(ArduinoJson::JsonVariantConst row, Snapshot& out,
              cfg::Provider provider);
bool mapEvent(ArduinoJson::JsonVariantConst row, Snapshot& out, Match& match,
              cfg::Provider provider);
}  // namespace football
