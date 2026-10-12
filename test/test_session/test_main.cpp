// Host acceptance for the Web login sessions (docs/NETWORK.md, "Web server and
// REST API"): several browsers at once, sliding expiry, least recently used
// replacement and revocation.

#include <unity.h>

#include <cstring>

#include "app_config.h"
#include "session_table.h"

namespace {

using web::SessionTable;
constexpr int64_t kLife = cfg::kSessionLifetimeMs;

// 32 hexadecimal characters derived from one letter.
const char* token(char c) {
  static char text[SessionTable::kTokenChars + 1];
  memset(text, c, SessionTable::kTokenChars);
  text[SessionTable::kTokenChars] = 0;
  return text;
}

void testSeveralBrowsersHoldSessions() {
  SessionTable table;
  const auto a = table.open(token('a'), 1000);
  const auto b = table.open(token('b'), 2000);
  TEST_ASSERT_NOT_EQUAL(0, a);
  TEST_ASSERT_NOT_EQUAL(a, b);
  TEST_ASSERT_EQUAL_UINT32(a, table.find(token('a'), 3000));
  TEST_ASSERT_EQUAL_UINT32(b, table.find(token('b'), 3000));
  TEST_ASSERT_EQUAL_UINT32(0, table.find(token('c'), 3000));
}

void testExpirySlidesWithUse() {
  SessionTable table;
  const auto a = table.open(token('a'), 0);
  // Used every 10 minutes: alive long after the first 15 minutes.
  for (int64_t now = 600000; now <= 3600000; now += 600000)
    TEST_ASSERT_EQUAL_UINT32(a, table.find(token('a'), now));
  // Idle for the whole lifetime: expired.
  TEST_ASSERT_EQUAL_UINT32(0, table.find(token('a'), 3600000 + kLife));
}

void testFindWithoutSlideKeepsExpiry() {
  SessionTable table;
  const auto a = table.open(token('a'), 0);
  TEST_ASSERT_EQUAL_UINT32(a, table.find(token('a'), kLife - 1, false));
  TEST_ASSERT_EQUAL_UINT32(0, table.find(token('a'), kLife));
}

void testLeastRecentlyUsedIsReplaced() {
  SessionTable table;
  const char names[] = {'a', 'b', 'c', 'd'};
  static_assert(sizeof(names) == cfg::kSessionSlots, "one name per slot");
  for (unsigned i = 0; i < sizeof(names); ++i)
    table.open(token(names[i]), 1000 + i);
  // 'a' is used again, so 'b' is now the least recently used.
  TEST_ASSERT_NOT_EQUAL(0, table.find(token('a'), 5000));
  table.open(token('e'), 6000);
  TEST_ASSERT_EQUAL_UINT32(0, table.find(token('b'), 6001));
  TEST_ASSERT_NOT_EQUAL(0, table.find(token('a'), 6001));
  TEST_ASSERT_NOT_EQUAL(0, table.find(token('c'), 6001));
  TEST_ASSERT_NOT_EQUAL(0, table.find(token('d'), 6001));
  TEST_ASSERT_NOT_EQUAL(0, table.find(token('e'), 6001));
}

void testExpiredSlotIsReusedBeforeALiveOne() {
  SessionTable table;
  table.open(token('a'), 0);
  for (char c : {'b', 'c', 'd'}) table.open(token(c), 500000);
  // 'a' expired at kLife; the new session must not evict a live one.
  table.open(token('e'), kLife + 1);
  for (char c : {'b', 'c', 'd', 'e'})
    TEST_ASSERT_NOT_EQUAL(0, table.find(token(c), kLife + 2));
}

void testCloseAndRevokeOthers() {
  SessionTable table;
  const auto a = table.open(token('a'), 0);
  const auto b = table.open(token('b'), 0);
  const auto c = table.open(token('c'), 0);
  table.close(b);
  TEST_ASSERT_EQUAL_UINT32(0, table.find(token('b'), 1));
  TEST_ASSERT_FALSE(table.alive(b, 1));
  table.closeOthers(a);
  TEST_ASSERT_TRUE(table.alive(a, 1));
  TEST_ASSERT_FALSE(table.alive(c, 1));
  table.clear();
  TEST_ASSERT_FALSE(table.alive(a, 1));
  TEST_ASSERT_FALSE(table.alive(0, 1));
}

void testMalformedTokensNeverMatch() {
  SessionTable table;
  table.open(token('a'), 0);
  TEST_ASSERT_EQUAL_UINT32(0, table.find(nullptr, 1));
  TEST_ASSERT_EQUAL_UINT32(0, table.find("", 1));
  TEST_ASSERT_EQUAL_UINT32(0, table.find("aaaa", 1));
  char longer[SessionTable::kTokenChars + 2];
  memset(longer, 'a', sizeof(longer));
  longer[sizeof(longer) - 1] = 0;
  TEST_ASSERT_EQUAL_UINT32(0, table.find(longer, 1));
}

void testReopenedSerialIsNewAndOldTokenIsGone() {
  SessionTable table;
  const auto a = table.open(token('a'), 0);
  table.close(a);
  const auto again = table.open(token('a'), 10);
  TEST_ASSERT_NOT_EQUAL(a, again);
  TEST_ASSERT_FALSE(table.alive(a, 11));
  TEST_ASSERT_TRUE(table.alive(again, 11));
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(testSeveralBrowsersHoldSessions);
  RUN_TEST(testExpirySlidesWithUse);
  RUN_TEST(testFindWithoutSlideKeepsExpiry);
  RUN_TEST(testLeastRecentlyUsedIsReplaced);
  RUN_TEST(testExpiredSlotIsReusedBeforeALiveOne);
  RUN_TEST(testCloseAndRevokeOthers);
  RUN_TEST(testMalformedTokensNeverMatch);
  RUN_TEST(testReopenedSerialIsNewAndOldTokenIsGone);
  return UNITY_END();
}
