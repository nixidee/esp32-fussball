#pragma once
#include <ArduinoJson.h>

#include "settings_model.h"
namespace web {
void settingsJson(ArduinoJson::JsonObject out, const settings::Model& model,
                  bool secrets = false);
// Applies a settings patch and validates the result. On failure *violation
// (if given) names the first rejected value: its path in this JSON and why.
bool applyJson(ArduinoJson::JsonObjectConst input, settings::Model& model,
               settings::Violation* violation = nullptr,
               const settings::Limits& limits = {});
// The competition store: {"competitions": [...], "fixture_pairs": [...]}.
void competitionsJson(ArduinoJson::JsonObject out,
                      const settings::Competitions& competitions);
// Applies a competitions patch: a list in the input replaces that list, an
// entry without id (or id 0) is new. Only shapes and text sizes are checked
// here; settings::saveCompetitions() assigns ids and validates the result.
bool applyCompetitionsJson(ArduinoJson::JsonObjectConst input,
                           settings::Competitions& competitions,
                           settings::Violation* violation = nullptr);
// Bounds, list maxima, text maxima and defaults; 'limits' are the maxima of
// the display profile.
void schemaJson(ArduinoJson::JsonObject out, const settings::Limits& limits);
}  // namespace web
