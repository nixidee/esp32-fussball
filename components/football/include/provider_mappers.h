#pragma once
#include <ArduinoJson.h>

#include <cstdint>

#include "football_model.h"
namespace football {
// Provider JSON → canonical model. Pure: no I/O and no ESP-IDF, so the
// native tests compile them with recorded responses.
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
