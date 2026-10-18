#pragma once
#include "json_stream.h"
#include "settings_model.h"
namespace web {
enum class SettingsDecode : uint8_t { kOk, kSyntax, kViolation };

// Object fields only. The caller owns the surrounding object and may compose
// application configuration, schema and export without another JSON encoder.
bool ssidValueJson(json::Writer&, std::string_view raw_bytes);
bool settingsJson(json::Writer&, const settings::Model&, bool secrets = false);
bool competitionsJson(json::Writer&, const settings::Competitions&);
bool competitionArrayJson(json::Writer&, const settings::Competitions&);
bool schemaJson(json::Writer&, const settings::Limits&, bool backlight);

// Complete request wrappers leave the supplied value unchanged on failure.
SettingsDecode exportOptions(json::Input, bool& secrets,
                             settings::Violation* = nullptr);
SettingsDecode applyJson(json::Input, settings::Model&,
                         settings::Violation* = nullptr,
                         const settings::Limits& = {});
SettingsDecode applyCompetitionsJson(json::Input, settings::Competitions&,
                                     settings::Violation* = nullptr);

// Nested codecs mutate caller-owned candidates only. Their caller must prove
// complete input and validate the whole application before any publication.
SettingsDecode applySettingsObject(json::Reader&, const json::Token&,
                                   settings::Model&, settings::Violation*,
                                   const settings::Limits& = {},
                                   const settings::FieldPath& = {});
SettingsDecode applyCompetitionArray(json::Reader&, const json::Token&,
                                     settings::Competitions&,
                                     settings::Violation*,
                                     const settings::FieldPath& = {});
}  // namespace web
