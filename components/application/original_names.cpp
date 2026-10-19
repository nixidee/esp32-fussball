#include "application_original_names.h"
#include "football_service.h"
#include "json_stream.h"
#include "original_names_channel.h"
#include "provider_client.h"
namespace application {
namespace {
struct OriginalEncoder {
  uint32_t query;
  const settings::Source& source;
  json::Writer writer;
  bool begun = false;
  char buffer[128]{};
  std::size_t used = 0;
  bool flush() {
    if (!used) return true;
    const bool ok = football::originals::write(query, {buffer, used});
    used = 0;
    return ok;
  }
  bool push(std::string_view bytes) {
    for (char byte : bytes) {
      buffer[used++] = byte;
      if (used == sizeof(buffer) && !flush()) return false;
    }
    return true;
  }
  OriginalEncoder(uint32_t id, const settings::Source& value)
      : query(id),
        source(value),
        writer({this, [](std::string_view bytes, void* p) {
                  auto& self = *static_cast<OriginalEncoder*>(p);
                  return self.push(bytes);
                }}) {}
  bool begin() {
    if (begun) return true;
    begun = true;
    return writer.object() && writer.field("query", query) &&
           writer.key("source") && writer.object() &&
           writer.field("provider", static_cast<unsigned>(source.provider)) &&
           writer.field("competition", source.competition.view()) &&
           writer.field("season", source.season.view()) && writer.end() &&
           writer.key("items") && writer.array();
  }
  football::provider::OriginalsSink sink() {
    return {
        this,
        [](void* p) {
          auto& self = *static_cast<OriginalEncoder*>(p);
          return self.begin() && self.writer.object();
        },
        [](uint32_t id, void* p) {
          return static_cast<OriginalEncoder*>(p)->writer.field("id", id);
        },
        [](football::provider::OriginalField field, void* p) {
          auto& writer = static_cast<OriginalEncoder*>(p)->writer;
          return writer.key(field == football::provider::OriginalField::kName
                                ? "name"
                                : "short_name") &&
                 writer.beginString();
        },
        [](std::string_view bytes, void* p) {
          return static_cast<OriginalEncoder*>(p)->writer.stringChunk(bytes);
        },
        [](football::provider::OriginalField, void* p) {
          return static_cast<OriginalEncoder*>(p)->writer.endString();
        },
        [](void* p) { return static_cast<OriginalEncoder*>(p)->writer.end(); }};
  }
  bool finish() {
    return begin() && writer.end() && writer.end() && writer.finish() &&
           flush();
  }
};
football::RequestOutcome encodeOriginalNames(
    uint32_t query, const settings::Source& source,
    football::OriginalNamesProducer produce, void* context) {
  OriginalEncoder encoder(query, source);
  const auto sink = encoder.sink();
  auto result = produce(sink, context);
  if (result.code == ESP_OK && !encoder.finish())
    result.fail(ESP_FAIL, football::Cause::kOther);
  return result;
}
}  // namespace
esp_err_t beginOriginalNames(uint8_t competition, uint32_t& query) {
  return football::beginOriginalNames(competition, query, encodeOriginalNames);
}
}  // namespace application
