// Image-reference maintenance has one production owner: the screen policy.
// These checks do not establish NVS, filesystem or device acceptance.
#include <unity.h>

#include <array>
#include <cstdint>

#include "screen_policy.h"
#include "settings_model.h"

namespace {
using settings::ImageMask;
using settings::screen_policy::Policy;
Policy policy;

constexpr ImageMask bit(uint8_t id) { return ImageMask{1} << id; }

void disabled_profiles_and_only_active_slides_reserve_their_images() {
  policy.appearance[0].image = 2;
  policy.appearance[2].image = 5;
  policy.slides = {3, 3, 7, 8, 10};
  policy.slide_count = 3;
  TEST_ASSERT_EQUAL_HEX16(bit(2) | bit(3) | bit(5) | bit(7),
                          settings::referencedImages(policy));
  policy.appearance[0].image = 200;
  policy.slides[0] = 0;
  TEST_ASSERT_EQUAL_HEX16(bit(3) | bit(5) | bit(7),
                          settings::referencedImages(policy));
}

void explicit_delete_removes_every_occurrence_and_only_the_selected_id() {
  policy.appearance[0].image = 2;
  policy.appearance[2].image = 5;
  policy.slides = {2, 3, 2, 5, 2};
  policy.slide_count = 5;
  policy.shared[0][0].visit_s = 123;
  policy.frame = true;
  policy.per_competition[4] = 1u << 2;
  Policy expected = policy;
  expected.appearance[0].image = 0;
  expected.slides = {3, 5, 0, 0, 0};
  expected.slide_count = 2;
  const ImageMask other_ids = settings::kLibraryImageMask & ~bit(2);
  TEST_ASSERT_TRUE(settings::dropMissingImages(policy, other_ids));
  TEST_ASSERT_TRUE(expected == policy);
  TEST_ASSERT_FALSE(settings::dropMissingImages(policy, other_ids));
  TEST_ASSERT_TRUE(expected == policy);
}

void repair_filters_all_missing_ids_stably_and_keeps_duplicate_survivors() {
  policy.appearance[0].image = 2;
  policy.appearance[2].image = 5;
  policy.slides = {5, 3, 3, 7, 2};
  policy.slide_count = 5;
  Policy expected = policy;
  expected.appearance[2].image = 0;
  expected.slides = {3, 3, 2, 0, 0};
  expected.slide_count = 3;
  TEST_ASSERT_TRUE(settings::dropMissingImages(policy, bit(2) | bit(3)));
  TEST_ASSERT_TRUE(expected == policy);
  TEST_ASSERT_TRUE(settings::dropMissingImages(policy, 0));
  TEST_ASSERT_EQUAL_UINT8(0, policy.slide_count);
  for (const auto& style : policy.appearance)
    TEST_ASSERT_EQUAL_UINT8(0, style.image);
  for (const uint8_t id : policy.slides) TEST_ASSERT_EQUAL_UINT8(0, id);
  TEST_ASSERT_FALSE(settings::dropMissingImages(policy, 0));
  TEST_ASSERT_TRUE(settings::screen_policy::validate(policy));
}

void retained_images_leave_every_policy_value_unchanged() {
  policy.appearance[0].image = 10;
  policy.slides = {10, 2, 10, 2, 10};
  policy.slide_count = 5;
  const Policy expected = policy;
  TEST_ASSERT_FALSE(
      settings::dropMissingImages(policy, settings::kLibraryImageMask));
  TEST_ASSERT_TRUE(expected == policy);
}

void policy_cleanup_rejects_invalid_library_references_consistently() {
  policy.appearance[0].image = 2;
  policy.appearance[2].image = 200;
  policy.slides = {2, 3, 11, 2, 0};
  policy.slide_count = 4;
  TEST_ASSERT_TRUE(settings::dropMissingImages(policy, bit(2)));
  TEST_ASSERT_EQUAL_HEX16(bit(2), settings::referencedImages(policy));
  TEST_ASSERT_EQUAL_UINT8(2, policy.slide_count);
  TEST_ASSERT_EQUAL_UINT8(2, policy.slides[0]);
  TEST_ASSERT_EQUAL_UINT8(2, policy.slides[1]);
  TEST_ASSERT_EQUAL_UINT8(0, policy.appearance[2].image);
  TEST_ASSERT_FALSE(settings::dropMissingImages(policy, bit(2)));
}

}  // namespace

void setUp() { policy = Policy{}; }
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(disabled_profiles_and_only_active_slides_reserve_their_images);
  RUN_TEST(explicit_delete_removes_every_occurrence_and_only_the_selected_id);
  RUN_TEST(repair_filters_all_missing_ids_stably_and_keeps_duplicate_survivors);
  RUN_TEST(retained_images_leave_every_policy_value_unchanged);
  RUN_TEST(policy_cleanup_rejects_invalid_library_references_consistently);
  return UNITY_END();
}
