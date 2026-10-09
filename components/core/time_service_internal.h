// Internal access for the device test of the time service (time_test.cpp).

#pragma once

#include "app_config.h"

namespace timekeeping::internal {

// Applies a zone directly, bypassing the settings; posts kTimeChanged if it
// changed. applySettings() restores the settings' zone. False: the C
// library could not store the rule (no memory), the zone is unchanged.
bool applyZone(const cfg::TimeZone& zone);

}  // namespace timekeeping::internal
