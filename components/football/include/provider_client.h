#pragma once
#include <ArduinoJson.h>

#include <cstddef>

#include "esp_err.h"
#include "football_model.h"
#include "settings_model.h"
namespace football {
// One shared capped budget for the request's field mask and the one element
// document released after mapping. An unfiltered request needs no mask.
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
// HTTP status (0 = none received), body bytes read, the stage the request
// reached ("dns", "tcp", "tls", "http") and the socket's pending error after
// a failed connect, for logging only.
struct FetchResult {
  int status = 0;
  std::size_t bytes = 0;
  const char* stage = "";
  int socket_error = 0;
};
// 'body_limit': the most body bytes read (cfg::kProviderBodyBytes, the
// owner's selection lists cfg::kSelectionBodyBytes, ESPN match summaries
// cfg::kProviderSummaryBodyBytes); a longer response fails.
// 'optional': a response without 'array_key' is valid (no elements), e.g.
// a provider that leaves out an empty list. 'enough': set by the consumer
// (through its context) when it needs no further element; the response ends
// there with success and its rest is not read.
esp_err_t fetchJson(const char* url, const char* array_key,
                    cfg::Provider provider, const settings::Model& model,
                    uint32_t generation, JsonConsumer consumer, void* context,
                    std::size_t body_limit = cfg::kProviderBodyBytes,
                    FetchResult* result = nullptr,
                    bool optional = false, const bool* enough = nullptr);
}  // namespace football
