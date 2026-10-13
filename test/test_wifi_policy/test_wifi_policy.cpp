// Host checks of the Wi-Fi policy's hold for a phone on the setup AP
// (network::WifiPolicy::poll): attempts wait while a client is connected,
// at most kWifiApClientHoldMs, and a new client starts a new hold. They
// prove only the policy, not the radio or a phone on the device. The
// reason mapping (W97) is checked for every state.

#include <unity.h>

#include <initializer_list>

#include "app_config.h"
#include "wifi_policy.h"

namespace {

using network::Command;
using network::State;
using network::WifiPolicy;

// A policy in AP fallback: the saved network was missing for longer than
// kWifiFallbackMs. Returns the time reached.
int64_t fallback(WifiPolicy& policy) {
  policy.start(true, 0);
  int64_t now = 0;
  TEST_ASSERT_EQUAL(Command::kScan, policy.poll(now, false, 0));
  policy.scanDone(true, false, now);
  while (!policy.ap()) {
    now += 1000;
    if (policy.poll(now, false, 0) == Command::kScan)
      policy.scanDone(true, false, now);
  }
  return now;
}

// The first scan while 'client' stays connected, polling every second from
// 'now'; -1 when none within 'limit' ms.
int64_t firstScan(WifiPolicy& policy, int64_t now, int64_t limit, bool client) {
  for (int64_t t = now; t <= now + limit; t += 1000)
    if (policy.poll(t, client, 0) == Command::kScan) return t;
  return -1;
}

void test_no_client_keeps_plan() {
  WifiPolicy policy;
  const auto now = fallback(policy);
  const auto scan = firstScan(policy, now, cfg::kWifiMissingScanMs, false);
  TEST_ASSERT_TRUE(scan >= 0);
}

void test_client_holds_at_most_limit() {
  WifiPolicy policy;
  const auto now = fallback(policy);
  const auto scan =
      firstScan(policy, now, cfg::kWifiApClientHoldMs + 60000, true);
  TEST_ASSERT_TRUE(scan >= now + cfg::kWifiApClientHoldMs);
  TEST_ASSERT_TRUE(scan <= now + cfg::kWifiApClientHoldMs +
                               cfg::kWifiMissingScanMs + 1000);
}

void test_after_limit_plan_continues_with_client() {
  WifiPolicy policy;
  auto now = fallback(policy);
  now = firstScan(policy, now, cfg::kWifiApClientHoldMs + 60000, true);
  policy.scanDone(true, false, now);
  const auto next = firstScan(policy, now + 1000, 60000, true);
  TEST_ASSERT_TRUE(next >= 0);
  TEST_ASSERT_TRUE(next <= now + cfg::kWifiMissingScanMs + 1000);
}

void test_client_leaving_and_returning_holds_again() {
  WifiPolicy policy;
  auto now = fallback(policy);
  now = firstScan(policy, now, cfg::kWifiApClientHoldMs + 60000, true);
  policy.scanDone(true, false, now);
  policy.poll(now + 1000, false, 0);  // the phone left
  const auto next = firstScan(policy, now + 2000, 60000, true);
  TEST_ASSERT_EQUAL_INT64(-1, next);
  TEST_ASSERT_EQUAL(State::kWaitSsid, policy.state());
}

void test_reason_of_every_state() {
  using network::offlineReason;
  using network::Reason;
  using network::reasonName;
  TEST_ASSERT_EQUAL(Reason::kNone, offlineReason(State::kConnected));
  TEST_ASSERT_EQUAL(Reason::kNotFound, offlineReason(State::kWaitSsid));
  TEST_ASSERT_EQUAL(Reason::kPassword, offlineReason(State::kBackoffAuth));
  TEST_ASSERT_EQUAL(Reason::kSetup, offlineReason(State::kApOnly));
  for (auto s :
       {State::kIdle, State::kScanning, State::kConnecting, State::kBackoff})
    TEST_ASSERT_EQUAL(Reason::kConnecting, offlineReason(s));
  TEST_ASSERT_EQUAL_STRING("", reasonName(Reason::kNone));
  TEST_ASSERT_EQUAL_STRING("connecting", reasonName(Reason::kConnecting));
  TEST_ASSERT_EQUAL_STRING("not_found", reasonName(Reason::kNotFound));
  TEST_ASSERT_EQUAL_STRING("password", reasonName(Reason::kPassword));
  TEST_ASSERT_EQUAL_STRING("setup_ap", reasonName(Reason::kSetup));
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_no_client_keeps_plan);
  RUN_TEST(test_client_holds_at_most_limit);
  RUN_TEST(test_after_limit_plan_continues_with_client);
  RUN_TEST(test_client_leaving_and_returning_holds_again);
  RUN_TEST(test_reason_of_every_state);
  return UNITY_END();
}
