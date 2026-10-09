// Host acceptance for the file service helpers (docs/ARCHITECTURE.md, "File
// service"): the flat name rule, bounded path building and the erased-flash
// check that decides whether storage may be initialised.

#include <unity.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "app_config.h"
#include "file_names.h"

namespace {

using files::buildPath;
using files::isErased;
using files::isValidName;
using files::kPathBufferBytes;

void testValidNames() {
  TEST_ASSERT_TRUE(isValidName("a"));
  TEST_ASSERT_TRUE(isValidName("crest.jpg"));
  TEST_ASSERT_TRUE(isValidName("bg_01-night.rgb565"));
  TEST_ASSERT_TRUE(isValidName("0"));
  TEST_ASSERT_TRUE(isValidName("a.."));
  TEST_ASSERT_TRUE(isValidName(std::string(cfg::kFileNameMaxChars, 'x')));
}

void testRejectedNames() {
  TEST_ASSERT_FALSE(isValidName(""));
  TEST_ASSERT_FALSE(isValidName(std::string(cfg::kFileNameMaxChars + 1, 'x')));
  // Leading dot: ".", "..", hidden and reserved temporary names.
  TEST_ASSERT_FALSE(isValidName("."));
  TEST_ASSERT_FALSE(isValidName(".."));
  TEST_ASSERT_FALSE(isValidName(".tmp"));
  // No directories, no traversal.
  TEST_ASSERT_FALSE(isValidName("a/b"));
  TEST_ASSERT_FALSE(isValidName("../a"));
  TEST_ASSERT_FALSE(isValidName("/a"));
  TEST_ASSERT_FALSE(isValidName("a\\b"));
  // Lower case only; no spaces, control or non-ASCII bytes.
  TEST_ASSERT_FALSE(isValidName("Crest.jpg"));
  TEST_ASSERT_FALSE(isValidName("a b"));
  TEST_ASSERT_FALSE(isValidName("a\tb"));
  TEST_ASSERT_FALSE(isValidName("\xC3\xA4"));
  TEST_ASSERT_FALSE(isValidName(std::string("a\0b", 3)));
}

void testBuildPath() {
  std::array<char, kPathBufferBytes> out{};
  TEST_ASSERT_TRUE(buildPath(cfg::kFsBasePath, "crest.jpg", out));
  TEST_ASSERT_EQUAL_STRING("/fs/crest.jpg", out.data());
}

void testLongestNameFitsPathBuffer() {
  std::array<char, kPathBufferBytes> out{};
  const std::string name(cfg::kFileNameMaxChars, 'x');
  TEST_ASSERT_TRUE(buildPath(cfg::kFsBasePath, name, out));
  TEST_ASSERT_EQUAL_STRING((std::string(cfg::kFsBasePath) + "/" + name).c_str(),
                           out.data());
  TEST_ASSERT_EQUAL_UINT(kPathBufferBytes - 1, std::strlen(out.data()));
}

void testBuildPathRejectsInvalidName() {
  std::array<char, kPathBufferBytes> out;
  out.fill('#');
  TEST_ASSERT_FALSE(buildPath(cfg::kFsBasePath, "../settings", out));
  TEST_ASSERT_EQUAL_STRING("", out.data());
}

void testBuildPathRejectsSmallBuffer() {
  // "/fs/ab" needs 7 bytes with terminator.
  std::array<char, 6> small;
  small.fill('#');
  TEST_ASSERT_FALSE(buildPath(cfg::kFsBasePath, "ab", small));
  TEST_ASSERT_EQUAL_STRING("", small.data());
  std::array<char, 7> exact{};
  TEST_ASSERT_TRUE(buildPath(cfg::kFsBasePath, "ab", exact));
  TEST_ASSERT_EQUAL_STRING("/fs/ab", exact.data());
}

void testBuildPathWithEmptyBuffer() {
  TEST_ASSERT_FALSE(buildPath(cfg::kFsBasePath, "ab", std::span<char>{}));
}

void testErased() {
  std::array<uint8_t, 256> bytes;
  bytes.fill(0xFF);
  TEST_ASSERT_TRUE(isErased(bytes));
  TEST_ASSERT_TRUE(isErased(std::span<const uint8_t>{}));
  // Any programmed bit in any position means "not empty".
  for (std::size_t i : {std::size_t{0}, std::size_t{128}, bytes.size() - 1}) {
    bytes[i] = 0xFE;
    TEST_ASSERT_FALSE(isErased(bytes));
    bytes[i] = 0xFF;
  }
  bytes.fill(0x00);
  TEST_ASSERT_FALSE(isErased(bytes));
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(testValidNames);
  RUN_TEST(testRejectedNames);
  RUN_TEST(testBuildPath);
  RUN_TEST(testLongestNameFitsPathBuffer);
  RUN_TEST(testBuildPathRejectsInvalidName);
  RUN_TEST(testBuildPathRejectsSmallBuffer);
  RUN_TEST(testBuildPathWithEmptyBuffer);
  RUN_TEST(testErased);
  return UNITY_END();
}
