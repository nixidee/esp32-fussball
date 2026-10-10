#pragma once
#include "esp_err.h"
#include "football_model.h"
namespace football {
struct Status {
  uint32_t revision, requests[4], generation;
  int64_t fetched_ms;
  uint8_t matches, rows;
  bool stale, busy;
  Text<96> error;
  Text<96> routing_notice;
};
struct SelectionRequest {
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  bool teams = false;
  Id competition, season;
  Text<48> search;
  Text<48> country;
  uint16_t offset = 0;
};
struct SelectionStatus {
  bool busy, more;
  uint16_t count;
  uint32_t revision;
  Text<96> error;
};
esp_err_t init();
void refresh();
Status status();
// Called while holding the short repository read mutex. Callback must copy
// into its own fixed model, with no I/O, LVGL, allocation or blocking calls.
using Reader = void (*)(const Snapshot&, void*);
void read(Reader reader, void* context);
esp_err_t select(const SelectionRequest& request);
SelectionStatus selectionStatus();
bool selectionAt(uint16_t index, uint32_t revision, Selection& out);
}  // namespace football
