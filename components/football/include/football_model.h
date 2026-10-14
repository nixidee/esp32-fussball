#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>

#include "app_config.h"

namespace football {
// Why a provider operation failed; the stable key of the error text for the
// Web UI (REST API). kNone = no error.
enum class Cause : uint8_t {
  kNone,
  kNotStarted,   // the service has not started
  kProviderOff,  // provider needs a key or opt-in that is not set
  kBudget,       // daily request budget used
  kBusy,         // another heavy operation held the network too long
  kNetwork,      // network lost or changed during the operation
  kTime,         // no valid time
  kSettings,     // settings changed during the operation
  kRateLimited,  // HTTP 429
  kAuth,         // HTTP 401 or 403
  kNotFound,     // HTTP 404
  kHttp,         // any other HTTP status than 200
  kConnect,      // no HTTP response: DNS, TCP or TLS failed
  kBadResponse,  // response missing, incomplete or unreadable
  kTooLarge,     // response or result exceeds the device's capacity
  kMemory,       // out of memory
  kOther
};
template <std::size_t N>
struct Text {
  char data[N + 1]{};
  void assign(std::string_view text) {
    std::size_t count = text.size() < N ? text.size() : N;
    while (count < text.size() && count > 0 &&
           (static_cast<unsigned char>(text[count]) & 0xc0) == 0x80)
      --count;
    std::memset(data, 0, sizeof(data));
    std::memcpy(data, text.data(), count);
  }
  bool empty() const { return data[0] == 0; }
  bool operator==(const Text&) const = default;
};
struct Id : Text<cfg::kIdBytes> {
  // IDs are identities: never accept a truncated prefix as a different ID.
  bool assign(std::string_view value) {
    if (value.size() > cfg::kIdBytes || value.find('\0') != value.npos) {
      data[0] = 0;
      return false;
    }
    Text<cfg::kIdBytes>::assign(value);
    return true;
  }
  bool operator==(const Id&) const = default;
};
enum class Phase : uint8_t {
  kUnknown,
  kScheduled,
  kLive,
  kHalfTime,
  kExtraTime,
  kPenalties,
  kFinished,
  kPostponed,
  kCancelled,
  kAbandoned
};
enum class EventType : uint8_t {
  kGoal,
  kOwnGoal,
  kPenaltyGoal,
  kPenaltyMissed,
  kYellow,
  kYellowRed,
  kRed,
  kSubstitution,
  kInjury,
  kVar
};
struct Selection {
  Id id, season;
  Text<cfg::kNameBytes> name;
};
const char* providerName(cfg::Provider provider);
// Stable lower-case key of a failure cause (REST API); "" for kNone.
const char* causeKey(Cause cause);
// Stable lower-case code of a match phase (REST API).
const char* phaseName(Phase phase);
int64_t parseUtc(const char* text);
// Phase of an API-Football short status or a football-data.org v4 match
// status (all eleven values of its lookup table); kUnknown otherwise.
// Suspended maps to postponed and awarded to abandoned, like API-Football's
// SUSP and AWD.
Phase statusPhase(const char* status);
uint32_t fingerprint(std::string_view value);
}  // namespace football
