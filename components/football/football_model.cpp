#include "football_model.h"

#include <cstring>

namespace football {
const char* providerName(cfg::Provider p) {
  switch (p) {
    case cfg::Provider::kOpenLigaDb: return "openligadb";
    case cfg::Provider::kApiFootball: return "api-football";
    case cfg::Provider::kEspn: return "espn";
    case cfg::Provider::kFootballData: return "football-data";
  }
  return "unknown";
}
const char* causeKey(Cause cause) {
  switch (cause) {
    case Cause::kNone: return "";
    case Cause::kNotStarted: return "not_started";
    case Cause::kProviderOff: return "provider_off";
    case Cause::kBudget: return "budget_day";
    case Cause::kBusy: return "busy";
    case Cause::kNetwork: return "net_lost";
    case Cause::kTime: return "time_invalid";
    case Cause::kSettings: return "settings_changed";
    case Cause::kRateLimited: return "rate_limited";
    case Cause::kAuth: return "auth";
    case Cause::kNotFound: return "not_found";
    case Cause::kHttp: return "http_error";
    case Cause::kConnect: return "connect";
    case Cause::kBadResponse: return "bad_response";
    case Cause::kTooLarge: return "too_large";
    case Cause::kMemory: return "memory";
    case Cause::kOther: return "other";
  }
  return "other";
}
const char* phaseName(Phase phase) {
  switch (phase) {
    case Phase::kUnknown: return "unknown";
    case Phase::kScheduled: return "scheduled";
    case Phase::kLive: return "live";
    case Phase::kHalfTime: return "half_time";
    case Phase::kExtraTime: return "extra_time";
    case Phase::kPenalties: return "penalties";
    case Phase::kFinished: return "finished";
    case Phase::kPostponed: return "postponed";
    case Phase::kCancelled: return "cancelled";
    case Phase::kAbandoned: return "abandoned";
  }
  return "unknown";
}
int64_t parseUtc(const char* text) {
  if (text == nullptr) return 0;
  const auto length = strlen(text);
  if (length < 16 || length > 40 || text[4] != '-' || text[7] != '-' ||
      text[10] != 'T' || text[13] != ':')
    return 0;
  auto digits = [&](unsigned start, unsigned count) {
    int result = 0;
    if (start + count > length) return -1;
    for (unsigned i = start; i < start + count; ++i) {
      if (text[i] < '0' || text[i] > '9') return -1;
      result = result * 10 + text[i] - '0';
    }
    return result;
  };
  int y = digits(0, 4), m = digits(5, 2), d = digits(8, 2);
  const int h = digits(11, 2), n = digits(14, 2);
  int s = 0;
  unsigned at = 16;
  if (text[at] == ':') {
    s = digits(at + 1, 2);
    at += 3;
  }
  if (y < 2020 || y > 2100 || m < 1 || m > 12 || d < 1 || h < 0 || h > 23 ||
      n < 0 || n > 59 || s < 0 || s > 59)
    return 0;
  const unsigned month_days[] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31};
  const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  if (static_cast<unsigned>(d) > month_days[m - 1] + (m == 2 && leap)) return 0;
  if (text[at] == '.') {
    const auto first = ++at;
    while (at < length && text[at] >= '0' && text[at] <= '9') ++at;
    if (at == first) return 0;
  }
  int offset = 0;
  if (text[at] == 'Z')
    ++at;
  else if (text[at] == '+' || text[at] == '-') {
    const int sign = text[at] == '+' ? 1 : -1;
    const int oh = digits(at + 1, 2), on = digits(at + 4, 2);
    if (at + 6 != length || text[at + 3] != ':' || oh < 0 || oh > 23 ||
        on < 0 || on > 59)
      return 0;
    offset = sign * (oh * 3600 + on * 60);
    at += 6;
  }
  if (at != length) return 0;
  // Gregorian days-to-epoch, independent of process TZ and libc timegm.
  y -= m <= 2;
  const int era = y / 400;
  const unsigned yo = y - era * 400;
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yo * 365 + yo / 4 - yo / 100 + doy;
  int64_t utc = (era * 146097LL + doe - 719468) * 86400 + h * 3600 + n * 60 + s;
  return utc - offset;
}
Phase statusPhase(const char* s) {
  if (!strcmp(s, "NS") || !strcmp(s, "TBD") || !strcmp(s, "TIMED") ||
      !strcmp(s, "SCHEDULED"))
    return Phase::kScheduled;
  if (!strcmp(s, "1H") || !strcmp(s, "2H") || !strcmp(s, "IN_PLAY") ||
      !strcmp(s, "LIVE"))
    return Phase::kLive;
  if (!strcmp(s, "HT") || !strcmp(s, "PAUSED")) return Phase::kHalfTime;
  if (!strcmp(s, "ET") || !strcmp(s, "EXTRA_TIME")) return Phase::kExtraTime;
  if (!strcmp(s, "P") || !strcmp(s, "PENALTY_SHOOTOUT"))
    return Phase::kPenalties;
  if (!strcmp(s, "FT") || !strcmp(s, "AET") || !strcmp(s, "PEN") ||
      !strcmp(s, "FINISHED"))
    return Phase::kFinished;
  if (!strcmp(s, "PST") || !strcmp(s, "POSTPONED") || !strcmp(s, "SUSP") ||
      !strcmp(s, "SUSPENDED"))
    return Phase::kPostponed;
  if (!strcmp(s, "CANC") || !strcmp(s, "CANCELLED")) return Phase::kCancelled;
  if (!strcmp(s, "ABD") || !strcmp(s, "AWD") || !strcmp(s, "ABANDONED") ||
      !strcmp(s, "AWARDED"))
    return Phase::kAbandoned;
  return Phase::kUnknown;
}
uint32_t fingerprint(std::string_view text) {
  uint32_t h = 2166136261;
  for (unsigned char c : text) h = (h ^ c) * 16777619;
  return h;
}
}  // namespace football
