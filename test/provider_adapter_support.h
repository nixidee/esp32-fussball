// Legacy recorded-fixture convenience for tests only. JSON variants never
// reach production mappers: serialize the selected fixture row and replay the
// real typed adapter, keeping existing behavior assertions unchanged.
#pragma once
#include <ArduinoJson.h>

#include <string>

#include "provider_adapter.h"
#include "store_mappers.h"
namespace football::test_support {
struct Input {
  std::string_view text;
  std::size_t at = 0;
  static int read(void* raw) {
    auto& input = *static_cast<Input*>(raw);
    return input.at < input.text.size()
               ? static_cast<unsigned char>(input.text[input.at++])
               : -1;
  }
};
inline std::string wrapped(ArduinoJson::JsonVariantConst row,
                           cfg::Provider provider, bool table) {
  std::string bytes;
  ArduinoJson::serializeJson(row, bytes);
  if (provider == cfg::Provider::kOpenLigaDb) return '[' + bytes + ']';
  if (provider == cfg::Provider::kEspn)
    return table ? "{\"standings\":{\"entries\":[" + bytes + "]}}"
                 : "{\"events\":[" + bytes + "]}";
  if (provider == cfg::Provider::kApiFootball)
    return "{\"response\":[" + bytes + "]}";
  return table ? "{\"standings\":[" + bytes + "]}"
               : "{\"matches\":[" + bytes + "]}";
}
}  // namespace football::test_support
namespace football {
// Only test overloads for historical fixture slicing. The firmware exports
// solely the typed overloads from store_mappers.h.
inline bool storeMatch(ArduinoJson::JsonVariantConst row, MatchStore& store,
                       const MapContext& map) {
  struct Context {
    MatchStore& store;
    const MapContext& map;
    bool accepted = false;
  } context{store, map};
  auto bytes = test_support::wrapped(row, store.provider(map.source), false);
  test_support::Input input{bytes};
  json::Reader reader({&input, test_support::Input::read});
  provider::RequestSpec request;
  request.source.provider = store.provider(map.source);
  provider::Sink sink;
  sink.context = &context;
  sink.fixture = [](const provider::Fixture& fixture,
                    std::span<const provider::Event> events, void* raw) {
    auto& c = *static_cast<Context*>(raw);
    return c.accepted = storeMatch(fixture, events, c.store, c.map);
  };
  provider::readResponse(reader, request, sink);
  return context.accepted;
}
inline bool storeTableRow(ArduinoJson::JsonVariantConst row, MatchStore& store,
                          const MapContext& map) {
  struct Context {
    MatchStore& store;
    const MapContext& map;
    bool accepted = true;
  } context{store, map};
  auto bytes = test_support::wrapped(row, store.provider(map.source), true);
  test_support::Input input{bytes};
  json::Reader reader({&input, test_support::Input::read});
  provider::RequestSpec request;
  request.source.provider = store.provider(map.source);
  request.operation = provider::Operation::kTable;
  provider::Sink sink;
  sink.context = &context;
  sink.row = [](const provider::TableRow& row, void* raw) {
    auto& c = *static_cast<Context*>(raw);
    auto mapped = row;
    if (c.store.provider(c.map.source) == cfg::Provider::kOpenLigaDb)
      mapped.facts.position = static_cast<uint8_t>(c.store.stagedRows() + 1);
    return c.accepted = storeTableRow(mapped, c.store, c.map);
  };
  provider::readResponse(reader, request, sink);
  return context.accepted;
}
}  // namespace football

#include "provider_plan.h"
namespace football::test_support {
inline provider::HttpRequest http(const plan::Request& request) {
  provider::HttpRequest out;
  provider::buildRequest(request.spec, out);
  return out;
}
inline provider::ParseResult feed(plan::Planner& planner,
                                  const provider::RequestSpec& request,
                                  std::string_view bytes) {
  Input input{bytes};
  json::Reader reader({&input, Input::read});
  provider::Sink sink;
  sink.context = &planner;
  sink.fixture = [](const provider::Fixture& fixture,
                    std::span<const provider::Event> events, void* raw) {
    return static_cast<plan::Planner*>(raw)->consume(fixture, events);
  };
  sink.row = [](const provider::TableRow& row, void* raw) {
    return static_cast<plan::Planner*>(raw)->consume(row);
  };
  sink.group = [](const provider::TableGroup& group, void* raw) {
    return static_cast<plan::Planner*>(raw)->consume(group);
  };
  sink.event = [](const provider::Event& event, void* raw) {
    return static_cast<plan::Planner*>(raw)->consume(event);
  };
  sink.day = [](uint32_t at, void* raw) {
    return static_cast<plan::Planner*>(raw)->consumeDay(at);
  };
  sink.calendar_round = [](const provider::CalendarRound& round, void* raw) {
    return static_cast<plan::Planner*>(raw)->consumeCalendarRound(round);
  };
  sink.text = [](std::string_view value, void* raw) {
    return static_cast<plan::Planner*>(raw)->consumeText(value);
  };
  auto result = provider::readResponse(reader, request, sink);
  planner.response(result);
  return result;
}
inline bool consume(ArduinoJson::JsonVariantConst row, plan::Planner& planner,
                    const settings::Source& source) {
  provider::RequestSpec request;
  request.source = {source.provider, source.competition.view(),
                    source.season.view()};
  auto bytes = wrapped(row, source.provider, false);
  // A sliced row has lost its response-level league proof. Restore the
  // explicitly supplied test envelope; never overwrite the row's own scope.
  if (source.provider == cfg::Provider::kEspn) {
    ArduinoJson::JsonDocument envelope;
    envelope["events"][0] = row;
    envelope["leagues"][0]["slug"] = source.competition.view();
    envelope["leagues"][0]["season"]["year"] = source.season.view();
    bytes.clear();
    ArduinoJson::serializeJson(envelope, bytes);
  }
  if (source.provider == cfg::Provider::kEspn &&
      row["participants"].is<ArduinoJson::JsonArrayConst>()) {
    request.operation = provider::Operation::kSummary;
    std::string value;
    ArduinoJson::serializeJson(row, value);
    bytes = "{\"keyEvents\":[" + value + "]}";
  }
  return feed(planner, request, bytes).status == provider::Status::kOk;
}
inline bool accepts(ArduinoJson::JsonVariantConst row, plan::Planner& planner,
                    const settings::Source& source) {
  provider::RequestSpec request;
  request.source = {source.provider, source.competition.view(),
                    source.season.view()};
  auto bytes = wrapped(row, source.provider, false);
  Input input{bytes};
  json::Reader reader({&input, Input::read});
  struct Context {
    plan::Planner& planner;
    bool accepted = false;
  } context{planner};
  provider::Sink sink;
  sink.context = &context;
  sink.fixture = [](const provider::Fixture& fixture,
                    std::span<const provider::Event>, void* raw) {
    auto& c = *static_cast<Context*>(raw);
    c.accepted = c.planner.acceptsMatch(fixture);
    return true;
  };
  provider::readResponse(reader, request, sink);
  return context.accepted;
}
}  // namespace football::test_support
