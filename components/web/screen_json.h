#pragma once
#include "json_stream.h"
#include "screen_policy.h"
#include "settings_json.h"

namespace web {
// Fields only; surrounding object belongs to the aggregate/export writer.
bool screenPolicyJson(json::Writer&, const settings::screen_policy::Policy&);
bool screenPolicySchemaJson(json::Writer&,
                            const settings::screen_policy::Limits& = {});
bool competitionPreferencesJson(
    json::Writer&, const settings::screen_policy::CompetitionPreferences&);

SettingsDecode applyScreenPolicyObject(
    json::Reader&, const json::Token&, settings::screen_policy::Policy&,
    settings::Violation* = nullptr, const settings::screen_policy::Limits& = {},
    const settings::FieldPath& = {});
SettingsDecode applyCompetitionPreferencesObject(
    json::Reader&, const json::Token&,
    settings::screen_policy::CompetitionPreferences&,
    settings::Violation* = nullptr, const settings::FieldPath& = {});
// Complete input wrapper; caller discards candidate on failure.
SettingsDecode applyScreenPolicyJson(
    json::Input, settings::screen_policy::Policy&,
    settings::Violation* = nullptr,
    const settings::screen_policy::Limits& = {});
void screenPolicyViolation(const settings::screen_policy::Error&,
                           settings::Violation&);
}  // namespace web
