// Typed provider query input, independent of HTTP, storage and rendering.
#pragma once
#include "football_model.h"
namespace football {
struct SelectionRequest {
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  bool teams = false;
  Id competition, season;
  Text<cfg::kSelectionTextMaxBytes> search;
  Text<cfg::kSelectionTextMaxBytes> country;
  uint16_t offset = 0;
};
}  // namespace football
