// Complete adopted-name collection decoder. It changes only an inactive
// candidate; the application owner alone publishes its reference through NVS.
#pragma once

#include "json_stream.h"
#include "settings_model.h"
#include "team_name_files.h"

namespace football::names {
enum class Decode : uint8_t {
  kOk,
  kSyntax,
  kViolation,
  kNoMemory,
  kStorageFull,
  kIo
};
// Caller begins candidate under heavy admission before decoding. The current
// token is the surrounding names object. Success consumes exactly that value
// and validates the complete FNS2 candidate; it does not finish the surrounding
// request or publish configuration. Abort the candidate on any later failure.
Decode readAdoptedJson(::json::Reader&, const ::json::Token&, files::Candidate&,
                       settings::Violation&);
}  // namespace football::names
