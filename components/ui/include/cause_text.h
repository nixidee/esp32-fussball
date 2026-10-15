#pragma once
#include "football_model.h"
namespace ui {
// Short display line for why a competition has no current data (OQ-280):
// fits the status band and the notice card of the round 240 px screen; the
// Web UI keeps its own fuller texts with what to do. "" for kNone.
const char* causeText(football::Cause cause, bool english);
}  // namespace ui
