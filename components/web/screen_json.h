#pragma once

#include <ArduinoJson.h>

#include <cstdint>

#include "screen_policy.h"
#include "settings_model.h"

namespace web {

// A complete screen settings representation. Phase/type names are independent
// of stored enum values and of the editable display order. Every rule is
// [enabled, automatic, visit_s]; null in a competition tuple inherits that one
// shared field. Collapsed competition rules remain present and editable.
void screenPolicyJson(ArduinoJson::JsonObject out,
                      const settings::screen_policy::Policy& policy,
                      uint32_t generation);

// Requires the entire representation, including generation. Zero is a valid
// boot generation. The settings service checks the supplied token. The caller
// owns one bounded candidate and must discard it on failure; this codec makes
// no policy copy and does not mutate any persistent state or image-library
// files.
bool applyScreenPolicyJson(ArduinoJson::JsonObjectConst input,
                           settings::screen_policy::Policy& candidate,
                           uint32_t& generation,
                           settings::Violation* violation = nullptr,
                           const settings::screen_policy::Limits& limits = {});

// Use the same actionable JSON field mapping for validation in the service.
void screenPolicyViolation(const settings::screen_policy::Error& error,
                           settings::Violation& violation);

// Compact metadata for the tuple fields, named identities, bounds and null
// inheritance. Kept out of each settings representation and PUT body.
void screenPolicySchemaJson(ArduinoJson::JsonObject out,
                            const settings::screen_policy::Limits& limits = {});

}  // namespace web
