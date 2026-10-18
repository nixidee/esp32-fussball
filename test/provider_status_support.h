// Provider status tests enter the actual typed adapter, not a second mapping.
#pragma once
#include <string>

#include "provider_adapter.h"
namespace football::test_support {
inline Phase mappedStatus(cfg::Provider provider, const char* status) {
  const bool api = provider == cfg::Provider::kApiFootball;
  const std::string body =
      api ? std::string(
                "{\"response\":[{\"fixture\":{\"id\":1,\"status\":{\"short\":"
                "\"") +
                status + "\"}}}]}"
          : std::string("{\"matches\":[{\"id\":1,\"status\":\"") + status +
                "\"}]}";
  struct Input {
    const std::string& body;
    std::size_t at = 0;
  } input{body};
  json::Reader reader({&input, [](void* raw) {
                         auto& in = *static_cast<Input*>(raw);
                         return in.at < in.body.size()
                                    ? static_cast<unsigned char>(
                                          in.body[in.at++])
                                    : -1;
                       }});
  Phase phase = Phase::kUnknown;
  provider::RequestSpec request;
  request.source.provider = provider;
  provider::Sink sink;
  sink.context = &phase;
  sink.fixture = [](const provider::Fixture& row,
                    std::span<const provider::Event>, void* raw) {
    *static_cast<Phase*>(raw) = row.facts.phase;
    return true;
  };
  provider::readResponse(reader, request, sink);
  return phase;
}
}  // namespace football::test_support
