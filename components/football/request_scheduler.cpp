#include "request_scheduler.h"

#include <algorithm>

namespace football::schedule {
namespace {

constexpr std::size_t kBuckets = cfg::kLimitWindowBuckets;
constexpr uint32_t kGapS = (cfg::kRequestGapMs + 999) / 1000;
constexpr uint32_t kDayS = 86400;

static_assert(kBuckets >= 2);

bool isLive(cfg::DataType type) {
  return type == cfg::DataType::kLive || type == cfg::DataType::kLiveTable ||
         type == cfg::DataType::kEvents;
}

// Live windows of one competition, merged, seen from 'utc'.
struct Activity {
  bool active = false;
  uint32_t start = 0;    // start of the window containing 'utc'
  uint32_t next = 0;     // start of the next window, 0 = none
  uint32_t ended = 0;    // end of the latest window that has ended, 0 = none
  uint32_t seconds = 0;  // window seconds between 'utc' and the day end
  uint32_t windows = 0;  // windows overlapping that time
};

void addWindow(Activity& a, uint32_t start, uint32_t end, int64_t utc,
               int64_t day_end) {
  if (end <= utc) {
    a.ended = std::max(a.ended, end);
  } else if (start <= utc) {
    a.active = true;
    a.start = start;
  } else if (a.next == 0) {
    a.next = start;
  }
  const int64_t from = std::max<int64_t>(start, utc);
  const int64_t to = std::min<int64_t>(end, day_end);
  if (to > from) {
    a.seconds += static_cast<uint32_t>(to - from);
    ++a.windows;
  }
}

Activity activity(const MatchStore& store, uint8_t competition, bool own,
                  int64_t utc, int64_t day_end) {
  Activity a;
  bool open = false;
  uint32_t first = 0, last = 0;
  // Matches are sorted by kickoff, so window starts are sorted too.
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (m.competition != competition || (own && !m.isOwn())) continue;
    uint32_t start = 0, end = 0;
    if (!liveWindow(m, start, end)) continue;
    if (open && start <= last) {
      last = std::max(last, end);
      continue;
    }
    if (open) addWindow(a, first, last, utc, day_end);
    open = true;
    first = start;
    last = end;
  }
  if (open) addWindow(a, first, last, utc, day_end);
  return a;
}

Class classOf(cfg::DataType type, bool own_active) {
  switch (type) {
    case cfg::DataType::kLiveTable: return Class::kLiveTable;
    case cfg::DataType::kTable: return Class::kTable;
    case cfg::DataType::kFixtures: return Class::kFixtures;
    case cfg::DataType::kLive: return own_active ? Class::kOwn : Class::kLive;
    case cfg::DataType::kEvents: return Class::kOwn;
  }
  return Class::kFixtures;
}

// Interval factor over the provider's and the global daily cap.
uint8_t combine(uint8_t a, uint8_t b) {
  return a == 0 || b == 0 ? 0 : std::max(a, b);
}

uint32_t clampS(uint64_t value) {
  return value >= UINT32_MAX ? UINT32_MAX - 1 : static_cast<uint32_t>(value);
}
// A due second from a start that may lie before boot (negative).
uint32_t dueS(int64_t value) {
  return value <= 0 ? 0 : clampS(static_cast<uint64_t>(value));
}

// Requests a job with interval 'interval' makes inside 'windows' windows of
// 'seconds' in total: one at each window start, then one per interval.
uint32_t windowDemand(uint32_t seconds, uint32_t windows, uint32_t interval) {
  return windows + seconds / interval;
}

}  // namespace

// ---------------------------------------------------------------- Window

void Window::set(uint16_t requests, uint16_t window_s) {
  if (requests == requests_ && window_s == window_s_) return;
  counts_.fill(0);
  newest_ = 0;
  requests_ = requests;
  window_s_ = window_s;
  width_ = limited() ? static_cast<uint16_t>(std::max<uint32_t>(
                           1, (window_s + kBuckets - 2) / (kBuckets - 1)))
                     : 1;
}

uint32_t Window::counted(uint32_t first, uint32_t last) const {
  const uint32_t oldest = newest_ >= kBuckets - 1 ? newest_ - (kBuckets - 1) : 0;
  first = std::max(first, oldest);
  last = std::min(last, newest_);
  uint32_t sum = 0;
  for (uint32_t k = first; k <= last && first <= last; ++k)
    sum += counts_[k % kBuckets];
  return sum;
}

uint32_t Window::admissibleAt(uint32_t now_s) const {
  if (!limited()) return now_s;
  // Buckets that overlap (now − W, now].
  uint32_t first = now_s >= window_s_ ? (now_s - window_s_) / width_ : 0;
  uint32_t sum = counted(first, now_s / width_);
  if (sum < requests_) return now_s;
  // Each bucket leaves the window W seconds after its end.
  for (uint32_t k = first; k <= newest_; ++k) {
    sum -= counted(k, k);
    const uint64_t leaves = (uint64_t{k} + 1) * width_ + window_s_;
    if (sum < requests_) return clampS(std::max<uint64_t>(leaves, now_s));
  }
  return clampS((uint64_t{newest_} + 1) * width_ + window_s_);
}

void Window::record(uint32_t now_s) {
  if (!limited()) return;
  const uint32_t k = std::max(now_s / width_, newest_);
  if (k > newest_) {
    const uint32_t clear = std::min<uint32_t>(k - newest_, kBuckets);
    for (uint32_t i = 1; i <= clear; ++i) counts_[(newest_ + i) % kBuckets] = 0;
    newest_ = k;
  }
  auto& count = counts_[k % kBuckets];
  if (count < UINT16_MAX) ++count;
}

// ------------------------------------------------------------- Scheduler

Scheduler::Scheduler() { reset(); }

void Scheduler::reset() {
  last_.fill(kNotStarted);
  failures_.fill(0);
  pending_.reset();
  ids_.fill(0);
  for (auto& w : windows_) w.set(0, 0);
  global_window_.set(0, 0);
  paused_until_.fill(0);
  last_start_ = kNever;
  deferred_until_ = 0;
  factors_.fill(1);
}

std::size_t Scheduler::index(uint8_t slot, cfg::DataType type) const {
  return static_cast<std::size_t>(slot) * cfg::kDataTypeCount +
         static_cast<std::size_t>(type);
}

bool Scheduler::degraded() const {
  return std::any_of(factors_.begin(), factors_.end(),
                     [](uint8_t f) { return f != 1; });
}

Decision Scheduler::next(const Inputs& in) {
  Decision decision;
  factors_.fill(1);
  if (in.plans == nullptr) return decision;
  const auto& plans = *in.plans;
  const uint32_t now = in.clock.mono_s;
  const int64_t utc = in.clock.utc;
  const int64_t day_end = (utc / kDayS + 1) * kDayS;
  const uint32_t day_left = static_cast<uint32_t>(day_end - utc);
  // Monotonic second of a UTC instant (0 for an instant before boot).
  const auto mono = [&](int64_t at) {
    const int64_t m = int64_t{now} + (at - utc);
    return m <= 0 ? uint32_t{0} : clampS(static_cast<uint64_t>(m));
  };

  configure(plans, in.global);

  std::array<Activity, cfg::kCompetitionMax> live{}, own{};
  for (uint8_t slot = 0; slot < cfg::kCompetitionMax; ++slot) {
    const auto& wanted = in.competitions[slot];
    if (ids_[slot] != wanted.id) {
      clear(slot);
      ids_[slot] = wanted.id;
    }
    if (wanted.id == 0 || in.store == nullptr) continue;
    live[slot] = activity(*in.store, wanted.id, false, utc, day_end);
    own[slot] = activity(*in.store, wanted.id, true, utc, day_end);
  }

  // Expected requests until the end of the UTC day per daily cap and class.
  using Demand = std::array<uint32_t, kClassCount>;
  std::array<Demand, cfg::kProviderCount> provider_demand{};
  Demand global_demand{};
  const auto each = [&](auto&& visit) {
    for (uint8_t slot = 0; slot < cfg::kCompetitionMax; ++slot) {
      const auto& wanted = in.competitions[slot];
      if (wanted.id == 0) continue;
      for (std::size_t t = 0; t < cfg::kDataTypeCount; ++t)
        if (wanted.types & (1u << t)) {
          Job job;
          job.slot = slot;
          job.competition = wanted.id;
          job.type = static_cast<cfg::DataType>(t);
          job.provider = wanted.provider;
          visit(job);
        }
    }
  };
  each([&](const Job& job) {
    const auto p = static_cast<std::size_t>(job.provider);
    const uint32_t interval =
        std::max<uint32_t>(1, plans[p].interval_s[static_cast<std::size_t>(job.type)]);
    const auto i = index(job.slot, job.type);
    const uint32_t cost = std::max<uint8_t>(
        1, in.competitions[job.slot].cost[static_cast<std::size_t>(job.type)]);
    Demand demand{};
    const auto& l = live[job.slot];
    const auto& o = own[job.slot];
    switch (job.type) {
      case cfg::DataType::kLive: {
        demand[static_cast<std::size_t>(Class::kOwn)] =
            windowDemand(o.seconds, o.windows, interval);
        demand[static_cast<std::size_t>(Class::kLive)] = windowDemand(
            l.seconds - std::min(l.seconds, o.seconds),
            l.windows - std::min(l.windows, o.windows), interval);
        break;
      }
      case cfg::DataType::kLiveTable:
        demand[static_cast<std::size_t>(Class::kLiveTable)] =
            windowDemand(l.seconds, l.windows, interval);
        break;
      case cfg::DataType::kEvents:
        demand[static_cast<std::size_t>(Class::kOwn)] =
            windowDemand(o.seconds, o.windows, interval);
        break;
      default: {
        const uint32_t first =
            last_[i] == kNotStarted ||
                    int64_t{last_[i]} + interval <= int64_t{now}
                ? 0
                : static_cast<uint32_t>(int64_t{last_[i]} + interval - now);
        demand[static_cast<std::size_t>(classOf(job.type, false))] =
            first >= day_left ? 0 : 1 + (day_left - first - 1) / interval;
      }
    }
    for (std::size_t c = 0; c < kClassCount; ++c) {
      provider_demand[p][c] += demand[c] * cost;
      global_demand[c] += demand[c] * cost;
    }
  });

  std::array<Factors, cfg::kProviderCount> provider_factors{};
  for (std::size_t p = 0; p < cfg::kProviderCount; ++p) {
    provider_factors[p].fill(1);
    // A used-up cap blocks every job of its scope until the day ends
    // (Wait::kDaily); degrading it would only hide that reason.
    if (in.daily_left[p] != kUnlimited && in.daily_left[p] != 0)
      provider_factors[p] = degrade(provider_demand[p], in.daily_left[p]);
  }
  Factors global_factors;
  global_factors.fill(1);
  if (in.global_daily_left != kUnlimited && in.global_daily_left != 0)
    global_factors = degrade(global_demand, in.global_daily_left);
  for (std::size_t c = 0; c < kClassCount; ++c) {
    factors_[c] = global_factors[c];
    for (std::size_t p = 0; p < cfg::kProviderCount; ++p)
      factors_[c] = combine(factors_[c], provider_factors[p][c]);
  }

  // When each provider may send its next request: device gap, deferral,
  // its pause, the daily caps and the window limits.
  std::array<uint32_t, cfg::kProviderCount> admit_at{};
  std::array<Wait, cfg::kProviderCount> admit_why{};
  for (std::size_t p = 0; p < cfg::kProviderCount; ++p) {
    Wait why = Wait::kIdle;
    uint32_t at = admissibleAt(static_cast<cfg::Provider>(p), now, &why);
    if ((in.daily_left[p] == 0 || in.global_daily_left == 0) &&
        mono(day_end) > at) {
      at = mono(day_end);
      why = Wait::kDaily;
    }
    admit_at[p] = at;
    admit_why[p] = why;
  }

  // Every wanted job gets a due time. Among the due jobs whose provider may
  // send now, the most important class wins, then the longest due; a due
  // job whose provider must wait does not hold back another provider.
  uint32_t wake = degraded() ? mono(day_end) : 0;
  const auto earlier = [&wake](uint32_t at) {
    if (at != 0 && (wake == 0 || at < wake)) wake = at;
  };
  bool found = false, blocked = false;
  Job best;
  Class best_class = Class::kLiveTable, blocked_class = Class::kLiveTable;
  uint32_t best_due = 0;
  Wait blocked_why = Wait::kIdle;
  each([&](const Job& job) {
    if (in.competitions[job.slot].held &
        (1u << static_cast<unsigned>(job.type)))
      return;
    const auto p = static_cast<std::size_t>(job.provider);
    const auto i = index(job.slot, job.type);
    const Class cls = classOf(job.type, own[job.slot].active);
    const uint8_t factor =
        combine(provider_factors[p][static_cast<std::size_t>(cls)],
                global_factors[static_cast<std::size_t>(cls)]);
    const bool pending = pending_.test(i);
    if (factor == 0 && !pending) return;
    const uint32_t interval = std::max<uint32_t>(
        1, plans[p].interval_s[static_cast<std::size_t>(job.type)]);
    const uint64_t stretched = uint64_t{interval} * factor;
    const uint8_t f = failures_[i];
    const uint64_t retry =
        f == 0
            ? stretched
            : std::min<uint64_t>(
                  uint64_t{cfg::kProviderErrorRetryS}
                      << std::min<uint8_t>(f - 1, 16),
                  std::max<uint64_t>(stretched, cfg::kProviderErrorRetryMaxS));
    const int64_t last = last_[i];
    uint32_t due = 0;
    bool due_known = false;
    if (pending) {
      due = now;
      due_known = true;
    } else if (!isLive(job.type)) {
      due = last == kNotStarted ? now
                                : dueS(last + static_cast<int64_t>(retry));
      due_known = true;
    } else {
      const auto& a =
          job.type == cfg::DataType::kEvents ? own[job.slot] : live[job.slot];
      if (a.active) {
        due = last == kNotStarted || last < int64_t{mono(a.start)}
                  ? now
                  : dueS(last + static_cast<int64_t>(retry));
        due_known = true;
      } else {
        // One more request after a window ended (final table, last
        // events), while the end is less than one interval ago. The end
        // stays unclamped: a window that ended before boot is as old as it
        // is, not one that ended at boot.
        const int64_t ended = int64_t{now} + (int64_t{a.ended} - utc);
        const bool final_fetch = job.type != cfg::DataType::kLive &&
                                 a.ended != 0 &&
                                 int64_t{now} < ended + interval;
        if (final_fetch && (last == kNotStarted || last < ended)) {
          due = now;
          due_known = true;
        } else if (final_fetch && f > 0 &&
                   last + static_cast<int64_t>(retry) < ended + interval) {
          due = dueS(last + static_cast<int64_t>(retry));
          due_known = true;
        }
        if (a.next != 0) {
          const uint32_t opens = mono(a.next);
          if (!due_known || opens < due) due = opens;
          due_known = true;
        }
      }
    }
    if (!due_known) return;
    if (due > now) {
      earlier(due);
      return;
    }
    if (admit_at[p] > now) {
      earlier(admit_at[p]);
      if (!blocked || cls > blocked_class) {
        blocked = true;
        blocked_class = cls;
        blocked_why = admit_why[p];
      }
      return;
    }
    if (!found || cls > best_class || (cls == best_class && due < best_due)) {
      found = true;
      best = job;
      best_class = cls;
      best_due = due;
    }
  });

  if (found) {
    decision.run = true;
    decision.job = best;
    return decision;
  }
  decision.wait = blocked ? blocked_why : Wait::kIdle;
  decision.wake_s = wake;
  return decision;
}

void Scheduler::refresh() {
  last_.fill(kNotStarted);
  failures_.fill(0);
}
void Scheduler::forget(uint8_t slot) {
  if (slot < cfg::kCompetitionMax) ids_[slot] = 0;
}
void Scheduler::clear(uint8_t slot) {
  for (std::size_t t = 0; t < cfg::kDataTypeCount; ++t) {
    const auto i = index(slot, static_cast<cfg::DataType>(t));
    last_[i] = kNotStarted;
    failures_[i] = 0;
    pending_.reset(i);
  }
}
void Scheduler::seed(uint8_t slot, uint8_t id, cfg::DataType type,
                     uint32_t age_s, uint32_t now_s) {
  if (slot >= cfg::kCompetitionMax || id == 0 || isLive(type)) return;
  if (ids_[slot] != id) {
    clear(slot);
    ids_[slot] = id;
  }
  const int64_t start = int64_t{now_s} - age_s;
  last_[index(slot, type)] = static_cast<int32_t>(
      std::clamp<int64_t>(start, INT32_MIN, kNotStarted - 1));
}

void Scheduler::configure(const settings::ProviderPlans& plans,
                          const settings::RequestLimit& global) {
  for (std::size_t p = 0; p < cfg::kProviderCount; ++p)
    windows_[p].set(plans[p].limit.window_requests, plans[p].limit.window_s);
  global_window_.set(global.window_requests, global.window_s);
}

uint32_t Scheduler::admissibleAt(cfg::Provider provider, uint32_t now_s,
                                 Wait* why) const {
  const auto p = static_cast<std::size_t>(provider);
  uint32_t at = now_s;
  Wait reason = Wait::kIdle;
  const auto block = [&](uint32_t until, Wait cause) {
    if (until > at) {
      at = until;
      reason = cause;
    }
  };
  if (last_start_ != kNever)
    block(clampS(uint64_t{last_start_} + kGapS), Wait::kGap);
  block(deferred_until_, Wait::kBusy);
  if (p < cfg::kProviderCount) {
    block(paused_until_[p], Wait::kPaused);
    block(windows_[p].admissibleAt(now_s), Wait::kWindow);
  }
  block(global_window_.admissibleAt(now_s), Wait::kWindow);
  if (why != nullptr) *why = reason;
  return at;
}

void Scheduler::sent(cfg::Provider provider, uint32_t now_s) {
  const auto p = static_cast<std::size_t>(provider);
  if (p >= cfg::kProviderCount) return;
  last_start_ = now_s;
  windows_[p].record(now_s);
  global_window_.record(now_s);
}

void Scheduler::finished(const Job& job, Outcome outcome, uint32_t at_s,
                         bool more) {
  if (outcome == Outcome::kDeferred) {
    // Nothing was decided about the job; the device may be free after the
    // gap.
    deferred_until_ = std::max(deferred_until_, clampS(uint64_t{at_s} + kGapS));
    return;
  }
  if (job.slot >= cfg::kCompetitionMax || ids_[job.slot] != job.competition)
    return;
  const auto i = index(job.slot, job.type);
  // The table and the live table are the same request: a start, a success
  // or a failure of one is one of the other (OQ-263 amendment).
  const auto twin = job.type == cfg::DataType::kTable
                        ? index(job.slot, cfg::DataType::kLiveTable)
                    : job.type == cfg::DataType::kLiveTable
                        ? index(job.slot, cfg::DataType::kTable)
                        : i;
  const bool continued = pending_.test(i);
  pending_.set(i, outcome == Outcome::kOk && more);
  // A run that made no request counts from now; a continued run keeps the
  // start of its last request.
  if (outcome != Outcome::kNothing || !continued)
    last_[i] = last_[twin] =
        static_cast<int32_t>(std::min<uint32_t>(at_s, kNotStarted - 1));
  if (outcome == Outcome::kNothing) return;
  const uint8_t failures = outcome == Outcome::kOk
                               ? 0
                               : static_cast<uint8_t>(std::min<unsigned>(
                                     failures_[i] + 1u, UINT8_MAX));
  failures_[i] = failures_[twin] = failures;
  if (outcome == Outcome::kRateLimited)
    paused_until_[static_cast<std::size_t>(job.provider)] =
        clampS(uint64_t{at_s} + cfg::kProviderRateRetryS);
}

Factors degrade(const std::array<uint32_t, kClassCount>& demand,
                uint32_t left) {
  Factors factors;
  factors.fill(1);
  uint64_t total = 0;
  for (const auto d : demand) total += d;
  for (std::size_t c = 0; c < kClassCount && total > left; ++c) {
    if (demand[c] == 0) continue;
    const uint64_t others = total - demand[c];
    if (others >= left) {
      factors[c] = 0;
      total = others;
      continue;
    }
    const uint64_t room = left - others;
    const uint64_t factor = (demand[c] + room - 1) / room;
    factors[c] = factor > UINT8_MAX ? 0 : static_cast<uint8_t>(factor);
    break;
  }
  return factors;
}

bool liveWindow(const StoredMatch& m, uint32_t& start, uint32_t& end) {
  if (m.kickoff == 0) return false;
  const uint32_t lead = cfg::kLiveLeadMinutes * 60;
  start = m.kickoff > lead ? m.kickoff - lead : 0;
  switch (m.phase) {
    case Phase::kPostponed:
    case Phase::kCancelled:
    case Phase::kAbandoned:
      return false;
    case Phase::kFinished:
      if (m.finished_at == 0) return false;
      end = std::max(m.finished_at, start);
      return true;
    default:
      break;
  }
  const bool reported_running =
      running(m.phase) && !(m.estimated & kEstimatedPhase);
  end = m.kickoff + (reported_running ? cfg::kUnknownMatchEndMinutes
                                      : cfg::kExpectedMatchMinutes) *
                        60u;
  return true;
}

}  // namespace football::schedule
