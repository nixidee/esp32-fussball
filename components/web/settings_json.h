#pragma once
#include <ArduinoJson.h>

#include "settings_model.h"
namespace web {
void settingsJson(ArduinoJson::JsonObject out, const settings::Model& model,
                  bool secrets = false);
bool applyJson(ArduinoJson::JsonObjectConst input, settings::Model& model);
void schemaJson(ArduinoJson::JsonObject out);
}  // namespace web
