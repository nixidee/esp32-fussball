// One application Apply envelope. Nested codecs only mutate caller-owned draft
// storage; application sessions own publication, HTTP owns complete transport.
#pragma once
#include "app_config_model.h"
#include "correlation_id.h"
#include "settings_json.h"

namespace web {
struct ApplyRequest {
  uint32_t expected_revision = 0;
  uint64_t apply_id = 0;
  bool names_changed = false;
};
using ReadNames = SettingsDecode (*)(json::Reader&, const json::Token&, void*,
                                     settings::Violation&);
SettingsDecode readApply(json::Input, settings::AppConfig&, ApplyRequest&,
                         settings::Violation&, ReadNames, void*,
                         const settings::Limits& = {});
using WriteNames = bool (*)(json::Writer&, void*);
bool exportJson(json::Writer&, const settings::AppConfig&, bool secrets,
                WriteNames, void*);
bool configJson(json::Writer&, const settings::AppConfig&,
                bool secrets = false);
}  // namespace web
