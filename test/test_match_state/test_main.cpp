// Host checks of the match state derived from provider status strings and
// of the error codes of failure causes (football_model.h, cause_text.h).
// They prove only this implementation, not the owner's device acceptance.

#include <unity.h>

#include <cstring>
#include <string>

#include "../provider_status_support.h"
#include "cause_text.h"
#include "football_model.h"

namespace {

using football::Phase;

void expectPhase(Phase expected, const char* status) {
  TEST_ASSERT_EQUAL_UINT_MESSAGE(
      static_cast<unsigned>(expected),
      static_cast<unsigned>(football::test_support::mappedStatus(
          cfg::Provider::kFootballData, status)),
      status);
}

// Every value of the football-data.org v4 match status lookup table.
void test_football_data_statuses() {
  expectPhase(Phase::kScheduled, "SCHEDULED");
  expectPhase(Phase::kScheduled, "TIMED");
  expectPhase(Phase::kLive, "IN_PLAY");
  expectPhase(Phase::kHalfTime, "PAUSED");
  expectPhase(Phase::kExtraTime, "EXTRA_TIME");
  expectPhase(Phase::kPenalties, "PENALTY_SHOOTOUT");
  expectPhase(Phase::kFinished, "FINISHED");
  expectPhase(Phase::kSuspended, "SUSPENDED");
  expectPhase(Phase::kPostponed, "POSTPONED");
  expectPhase(Phase::kCancelled, "CANCELLED");
  expectPhase(Phase::kAbandoned, "AWARDED");
}

void test_unknown_status_stays_unknown() {
  expectPhase(Phase::kUnknown, "");
  expectPhase(Phase::kUnknown, "in_play");
  expectPhase(Phase::kUnknown, "SOMETHING_NEW");
}

using football::Cause;

Cause causeAt(std::size_t n) { return static_cast<Cause>(n); }

// OQ-279: the numbers of the short error code are fixed once; the owner
// reads them on the display and finds them in the documentation.
void test_error_code_numbers_are_fixed() {
  const char* keys[] = {"",
                        "provider_off",
                        "budget_day",
                        "busy",
                        "net_lost",
                        "time_invalid",
                        "settings_changed",
                        "rate_limited",
                        "auth",
                        "not_found",
                        "http_error",
                        "connect",
                        "bad_response",
                        "too_large",
                        "memory",
                        "other",
                        "unmaintained",
                        "no_season",
                        "counter",
                        "store_full"};
  static_assert(sizeof(keys) / sizeof(keys[0]) == football::kCauseCount);
  for (std::size_t n = 0; n < football::kCauseCount; ++n) {
    TEST_ASSERT_EQUAL_UINT8(n, football::causeNumber(causeAt(n)));
    TEST_ASSERT_EQUAL_STRING(keys[n], football::causeKey(causeAt(n)));
  }
}

// OQ-279: provider abbreviation and number, the HTTP status after a dot.
void test_error_code_format() {
  char code[football::kErrorCodeBytes];
  football::errorCode(code, cfg::Provider::kOpenLigaDb, Cause::kRateLimited, 0);
  TEST_ASSERT_EQUAL_STRING("OLG E07", code);
  football::errorCode(code, cfg::Provider::kApiFootball, Cause::kAuth, 403);
  TEST_ASSERT_EQUAL_STRING(
      "APF E08\xC2\xB7"
      "403",
      code);
  football::errorCode(code, cfg::Provider::kEspn, Cause::kHttp, 503);
  TEST_ASSERT_EQUAL_STRING(
      "ESPN E10\xC2\xB7"
      "503",
      code);
  football::errorCode(code, cfg::Provider::kFootballData, Cause::kCounter,
                      65535);
  TEST_ASSERT_EQUAL_STRING(
      "FDO E18\xC2\xB7"
      "65535",
      code);
  football::errorCode(code, cfg::Provider::kEspn, Cause::kNone, 0);
  TEST_ASSERT_EQUAL_STRING("", code);
}

// OQ-280: every cause has a short display text in German and in English;
// no cause shows an empty line.
void test_every_cause_has_display_texts() {
  TEST_ASSERT_EQUAL_STRING("", ui::causeText(Cause::kNone, false));
  for (std::size_t n = 1; n < football::kCauseCount; ++n)
    for (const bool english : {false, true}) {
      const char* text = ui::causeText(causeAt(n), english);
      TEST_ASSERT_NOT_NULL(text);
      TEST_ASSERT_TRUE_MESSAGE(std::strlen(text) > 0,
                               football::causeKey(causeAt(n)));
    }
}

// OQ-288: every cause of a failed provider request (E07-E15) follows from
// the HTTP status and the transport result as the provider service maps
// them; the error code carries the status the owner reads on the display.
void test_request_failures_map_to_causes() {
  using football::fetchCause;
  using football::FetchError;
  struct Case {
    FetchError error;
    int status;
    Cause cause;
    const char* code;  // ESPN error code of this failure
  };
  const Case cases[] = {
      {FetchError::kOther, 429, Cause::kRateLimited,
       "ESPN E07\xC2\xB7"
       "429"},
      {FetchError::kOther, 401, Cause::kAuth,
       "ESPN E08\xC2\xB7"
       "401"},
      {FetchError::kOther, 403, Cause::kAuth,
       "ESPN E08\xC2\xB7"
       "403"},
      {FetchError::kOther, 404, Cause::kNotFound,
       "ESPN E09\xC2\xB7"
       "404"},
      {FetchError::kOther, 503, Cause::kHttp,
       "ESPN E10\xC2\xB7"
       "503"},
      {FetchError::kOther, 301, Cause::kHttp,
       "ESPN E10\xC2\xB7"
       "301"},
      {FetchError::kOther, 204, Cause::kHttp, "ESPN E10"},
      {FetchError::kOther, 0, Cause::kConnect, "ESPN E11"},
      // A response that arrived but could not be used: the transport result
      // decides, whatever the status.
      {FetchError::kBadResponse, 200, Cause::kBadResponse, "ESPN E12"},
      {FetchError::kBadResponse, 0, Cause::kBadResponse, "ESPN E12"},
      {FetchError::kTooLarge, 200, Cause::kTooLarge, "ESPN E13"},
      {FetchError::kNoMemory, 0, Cause::kMemory, "ESPN E14"},
      {FetchError::kNoMemory, 503, Cause::kMemory,
       "ESPN E14\xC2\xB7"
       "503"},
      {FetchError::kOther, 200, Cause::kOther, "ESPN E15"},
  };
  for (const auto& c : cases) {
    const Cause cause = fetchCause(c.error, c.status);
    TEST_ASSERT_EQUAL_STRING(football::causeKey(c.cause),
                             football::causeKey(cause));
    char code[football::kErrorCodeBytes];
    football::errorCode(code, cfg::Provider::kEspn, cause,
                        football::errorStatus(c.status));
    TEST_ASSERT_EQUAL_STRING(c.code, code);
  }
  // Failures outside a sent request keep the transport class only.
  TEST_ASSERT_TRUE(football::errorCause(FetchError::kNoMemory) ==
                   Cause::kMemory);
  TEST_ASSERT_TRUE(football::errorCause(FetchError::kTooLarge) ==
                   Cause::kTooLarge);
  TEST_ASSERT_TRUE(football::errorCause(FetchError::kBadResponse) ==
                   Cause::kBadResponse);
  TEST_ASSERT_TRUE(football::errorCause(FetchError::kOther) == Cause::kOther);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_football_data_statuses);
  RUN_TEST(test_unknown_status_stays_unknown);
  RUN_TEST(test_error_code_numbers_are_fixed);
  RUN_TEST(test_error_code_format);
  RUN_TEST(test_every_cause_has_display_texts);
  RUN_TEST(test_request_failures_map_to_causes);
  return UNITY_END();
}
