#include "matchday.h"

namespace football::matchday {
namespace {

// Local date (days since 1970-01-01) of 'utc' at 'offset'.
int64_t localDay(int64_t utc, int32_t offset) {
  const int64_t t = utc + offset;
  return t >= 0 ? t / 86400 : (t - 86399) / 86400;
}

}  // namespace

bool starts(uint32_t previous, uint32_t next) {
  return next - previous >= cfg::kMatchdayGapHours * 3600u;
}

bool over(uint32_t last_kickoff, int64_t utc, int32_t local_offset_s) {
  return localDay(utc, local_offset_s) >=
         localDay(last_kickoff, local_offset_s) + 1;
}

bool passed(uint32_t last_kickoff, int64_t utc, int32_t local_offset_s) {
  return localDay(utc, local_offset_s) >=
         localDay(last_kickoff, local_offset_s) + 2;
}

}  // namespace football::matchday
