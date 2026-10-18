#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>

#include "app_config.h"
#include "value_utils.h"

namespace football {
// Why a provider operation failed, or why a competition's data are not
// usable; the stable key of the error texts of the display and the Web UI
// (REST API) and, with causeNumber(), of the short error code. kNone = no
// error.
enum class Cause : uint8_t {
  kNone,
  kProviderOff,   // provider needs a key or opt-in that is not set
  kBudget,        // daily request budget used
  kBusy,          // another heavy operation held the network too long
  kNetwork,       // network lost or changed during the operation
  kTime,          // no valid time
  kSettings,      // settings changed during the operation
  kRateLimited,   // HTTP 429
  kAuth,          // HTTP 401 or 403
  kNotFound,      // HTTP 404
  kHttp,          // any other HTTP status than 200
  kConnect,       // no HTTP response: DNS, TCP or TLS failed
  kBadResponse,   // response missing, incomplete or unreadable
  kTooLarge,      // response or result exceeds the device's capacity
  kMemory,        // out of memory
  kOther,         // any other failure
  kUnmaintained,  // the provider leaves past matches without a result
  kNoSeason,      // the provider reports no current season
  kCounter,       // the daily request counter could not be stored
  kStoreFull      // the device could not hold all data of the competition
};
// Number of causes, kNone included (tests walk every cause).
inline constexpr std::size_t kCauseCount =
    static_cast<std::size_t>(Cause::kStoreFull) + 1;
// A state of the stored data, not a failed request (kUnmaintained,
// kStoreFull): the data stay shown with a warning, there is no retry.
inline constexpr bool dataNotice(Cause cause) {
  return cause == Cause::kUnmaintained || cause == Cause::kStoreFull;
}
template <std::size_t N>
struct Text {
  char data[N + 1]{};
  void assign(std::string_view text) {
    const auto count = values::utf8PrefixBytes(text, N);
    values::copyPadded(data, sizeof(data), {text.data(), count});
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
  kAbandoned,
  // An ongoing match is interrupted. This is distinct from a postponed
  // fixture: its previously justified live window may remain active.
  kSuspended
};
// Provider-reported period; elapsed minutes alone do not establish it.
enum class Period : uint8_t {
  kUnknown,
  kFirstHalf,
  kSecondHalf,
  kExtraFirstHalf,
  kExtraSecondHalf,
  kShootout
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
// Number of a cause in the short error code (1..; 0 for kNone). Assigned
// once and never changed (docs/API.md "Error codes").
uint8_t causeNumber(Cause cause);
// Short error code "OLG E11" of a cause, with the HTTP status when there is
// one ("OLG E10·503"); "" for kNone. 'http' 0 = none.
inline constexpr std::size_t kErrorCodeBytes = 16;
void errorCode(char (&out)[kErrorCodeBytes], cfg::Provider provider,
               Cause cause, uint16_t http);
// Class of a failed provider operation's error, independent of ESP-IDF (the
// provider service converts its esp_err_t: ESP_ERR_NO_MEM,
// ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_RESPONSE, anything else).
enum class FetchError : uint8_t { kNoMemory, kTooLarge, kBadResponse, kOther };
// Cause of a failure outside a sent request: memory, too large, bad response,
// else kOther.
Cause errorCause(FetchError error);
// Cause of a failed request that was sent, from its error class and the HTTP
// status it received (0 = no response): the error class first, then 429,
// 401/403, 404, no response, any other status than 200; kOther after a 200.
Cause fetchCause(FetchError error, int status);
// HTTP status shown in the error code of a failed sent request: 300..65535,
// else 0 (no response, or a 2xx response that could not be used).
uint16_t errorStatus(int status);
// Stable lower-case code of a match phase (REST API).
const char* phaseName(Phase phase);
int64_t parseUtc(const char* text);
uint32_t fingerprint(std::string_view value);
}  // namespace football
