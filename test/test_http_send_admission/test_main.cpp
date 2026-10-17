// The real transport's first-send boundary. This verifies callback charging
// and lifetime behavior; FreeRTOS/TLS/device timing remains a separate check.
#include <unity.h>

#include <array>

#include "http_send_admission.h"

namespace {
struct Attempt {
  football::http::SendAdmission admission;
  int reservations = 0, writes = 0, releases = 0;
  int refused = 0, result = 1;
  bool held = false;

  int write() {
    return admission.write(
        [&] {
          if (refused) return refused;
          TEST_ASSERT_FALSE(held);
          held = true;
          ++reservations;
          return 0;
        },
        [&] {
          // The first attempt owns the admission lock; later writes never do.
          TEST_ASSERT_EQUAL(writes == 0, held);
          ++writes;
          return result;
        },
        [&] {
          TEST_ASSERT_TRUE(held);
          held = false;
          ++releases;
        });
  }
};

void frame_during_dns_or_tls_sends_and_reserves_nothing() {
  Attempt attempt;
  TEST_ASSERT_TRUE(attempt.admission.allowed(false, true));
  TEST_ASSERT_FALSE(attempt.admission.allowed(true, true));
  TEST_ASSERT_EQUAL_INT(-1, attempt.write());
  TEST_ASSERT_EQUAL_INT(0, attempt.reservations);
  TEST_ASSERT_EQUAL_INT(0, attempt.writes);
  TEST_ASSERT_EQUAL_INT(0, attempt.releases);
  TEST_ASSERT_TRUE(attempt.admission.paused());
  TEST_ASSERT_FALSE(attempt.admission.started());
  // Frame exit schedules a fresh due attempt; it does not revive a partly
  // cancelled DNS/TLS transaction or pretend a provider failure happened.
  TEST_ASSERT_FALSE(attempt.admission.allowed(false, true));
  TEST_ASSERT_EQUAL_INT(0, attempt.admission.refusal());
}

void already_started_request_continues_without_recharging_or_holding_lock() {
  Attempt attempt;
  TEST_ASSERT_TRUE(attempt.admission.allowed(false, true));
  TEST_ASSERT_EQUAL_INT(1, attempt.write());
  TEST_ASSERT_TRUE(attempt.admission.started());
  TEST_ASSERT_FALSE(attempt.held);
  TEST_ASSERT_TRUE(attempt.admission.allowed(true, true));
  TEST_ASSERT_EQUAL_INT(1, attempt.write());
  TEST_ASSERT_EQUAL_INT(1, attempt.write());
  TEST_ASSERT_EQUAL_INT(1, attempt.reservations);
  TEST_ASSERT_EQUAL_INT(3, attempt.writes);
  TEST_ASSERT_EQUAL_INT(1, attempt.releases);
  TEST_ASSERT_FALSE(attempt.admission.paused());
}

void failed_zero_and_want_writes_release_and_count_the_begun_attempt_once() {
  // Error and WANT values are opaque to the admission mechanism; the real
  // TLS transport decides whether to retry them after releasing the lock.
  for (const int initial : std::array{-1, 0, -0x6900, -0x6880}) {
    Attempt attempt;
    attempt.result = initial;
    TEST_ASSERT_EQUAL_INT(initial, attempt.write());
    TEST_ASSERT_FALSE(attempt.held);
    TEST_ASSERT_EQUAL_INT(1, attempt.reservations);
    TEST_ASSERT_EQUAL_INT(1, attempt.releases);
    TEST_ASSERT_TRUE(attempt.admission.allowed(true, true));
    attempt.result = 10;
    TEST_ASSERT_EQUAL_INT(10, attempt.write());
    TEST_ASSERT_EQUAL_INT(1, attempt.reservations);
    TEST_ASSERT_EQUAL_INT(1, attempt.releases);
  }
}

void local_admission_refusal_never_reaches_transport_or_charges_a_request() {
  Attempt attempt;
  TEST_ASSERT_TRUE(attempt.admission.allowed(false, true));
  // Models frame winning the mutex race after the earlier DNS/TLS check,
  // a busy writer, a changed generation or a used request cap.
  attempt.refused = 123;
  TEST_ASSERT_EQUAL_INT(-1, attempt.write());
  TEST_ASSERT_EQUAL_INT(123, attempt.admission.refusal());
  TEST_ASSERT_FALSE(attempt.admission.started());
  TEST_ASSERT_FALSE(attempt.admission.paused());
  TEST_ASSERT_EQUAL_INT(0, attempt.reservations);
  TEST_ASSERT_EQUAL_INT(0, attempt.writes);
  TEST_ASSERT_EQUAL_INT(0, attempt.releases);
  TEST_ASSERT_FALSE(attempt.held);
}

void explicit_setup_bypasses_frame_but_still_requires_ordinary_admission() {
  Attempt setup;
  TEST_ASSERT_TRUE(setup.admission.allowed(true, false));
  TEST_ASSERT_EQUAL_INT(1, setup.write());
  TEST_ASSERT_EQUAL_INT(1, setup.reservations);
  TEST_ASSERT_EQUAL_INT(1, setup.releases);
  Attempt limited_setup;
  TEST_ASSERT_TRUE(limited_setup.admission.allowed(true, false));
  limited_setup.refused = 456;
  TEST_ASSERT_EQUAL_INT(-1, limited_setup.write());
  TEST_ASSERT_EQUAL_INT(0, limited_setup.writes);
  TEST_ASSERT_EQUAL_INT(0, limited_setup.reservations);
  TEST_ASSERT_EQUAL_INT(456, limited_setup.admission.refusal());
}

void zero_timeout_poll_and_eagain_retry_outside_the_first_send_lock() {
  constexpr int want_read = -100, want_write = -101;
  const std::array results{0, -1, want_read, want_write};
  const std::array errors{0, EAGAIN, 0, 0};
  for (unsigned i = 0; i < results.size(); ++i) {
    Attempt attempt;
    attempt.result = results[i];
    const int result = attempt.write();
    TEST_ASSERT_FALSE(attempt.held);
    TEST_ASSERT_TRUE(
        football::http::retryWrite(result, errors[i], want_read, want_write));
    // The outer poll can now wait while settings/frame changes proceed.
    TEST_ASSERT_TRUE(attempt.admission.allowed(true, true));
    attempt.result = 20;
    TEST_ASSERT_EQUAL_INT(20, attempt.write());
    TEST_ASSERT_EQUAL_INT(1, attempt.reservations);
    TEST_ASSERT_EQUAL_INT(1, attempt.releases);
  }
  TEST_ASSERT_TRUE(
      football::http::retryWrite(-1, EWOULDBLOCK, want_read, want_write));
  TEST_ASSERT_FALSE(football::http::retryWrite(-1, EIO, want_read, want_write));
  TEST_ASSERT_FALSE(
      football::http::retryWrite(-200, EAGAIN, want_read, want_write));
}
}  // namespace

void setUp() {}
void tearDown() {}
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(frame_during_dns_or_tls_sends_and_reserves_nothing);
  RUN_TEST(
      already_started_request_continues_without_recharging_or_holding_lock);
  RUN_TEST(
      failed_zero_and_want_writes_release_and_count_the_begun_attempt_once);
  RUN_TEST(
      local_admission_refusal_never_reaches_transport_or_charges_a_request);
  RUN_TEST(explicit_setup_bypasses_frame_but_still_requires_ordinary_admission);
  RUN_TEST(zero_timeout_poll_and_eagain_retry_outside_the_first_send_lock);
  return UNITY_END();
}
