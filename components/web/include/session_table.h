// Login sessions of the Web API (docs/NETWORK.md). Pure C++ without ESP-IDF
// calls: the caller supplies the token text and the monotonic time, so the
// rules run in the host tests.
//
// Up to cfg::kSessionSlots browsers hold a session at the same time. A new
// session replaces the one used longest ago when every slot is taken. Each
// accepted request moves the expiry; saving settings does not end sessions.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "app_config.h"

namespace web {
class SessionTable {
 public:
  static constexpr std::size_t kTokenChars = 32;
  // Identifies a session for as long as it lives; 0 is "no session".
  using Serial = uint32_t;

  // Starts a session for `token` (kTokenChars characters).
  Serial open(const char* token, int64_t now) {
    Slot* slot = nullptr;
    for (auto& item : slots_)
      if (!item.serial || now >= item.expires) {
        slot = &item;
        break;
      }
    if (!slot) {
      slot = &slots_[0];
      for (auto& item : slots_)
        if (item.used < slot->used) slot = &item;
    }
    memcpy(slot->token, token, kTokenChars);
    slot->token[kTokenChars] = 0;
    slot->used = now;
    slot->expires = now + cfg::kSessionLifetimeMs;
    if (++counter_ == 0) ++counter_;  // 0 means "no session"
    slot->serial = counter_;
    return slot->serial;
  }
  // The live session of `token`; `slide` moves its expiry. 0: unknown or
  // expired. Every slot is compared in constant time.
  Serial find(const char* token, int64_t now, bool slide = true) {
    if (!token || strnlen(token, kTokenChars + 1) != kTokenChars) return 0;
    Slot* found = nullptr;
    for (auto& item : slots_)
      if (item.serial && now < item.expires && same(item.token, token))
        found = &item;
    if (!found) return 0;
    if (slide) {
      found->used = now;
      found->expires = now + cfg::kSessionLifetimeMs;
    }
    return found->serial;
  }
  // True while the session exists and has not expired (no slide).
  bool alive(Serial serial, int64_t now) const {
    for (const auto& item : slots_)
      if (serial && item.serial == serial) return now < item.expires;
    return false;
  }
  void close(Serial serial) {
    for (auto& item : slots_)
      if (serial && item.serial == serial) item = {};
  }
  // Ends every session except `keep` (a new admin password).
  void closeOthers(Serial keep) {
    for (auto& item : slots_)
      if (item.serial != keep) item = {};
  }
  void clear() { closeOthers(0); }

 private:
  struct Slot {
    char token[kTokenChars + 1]{};
    int64_t expires = 0, used = 0;
    Serial serial = 0;
  };
  static bool same(const char* stored, const char* supplied) {
    uint8_t difference = 0;
    for (std::size_t i = 0; i < kTokenChars; ++i)
      difference |= static_cast<uint8_t>(stored[i] ^ supplied[i]);
    return difference == 0;
  }
  std::array<Slot, cfg::kSessionSlots> slots_{};
  Serial counter_ = 0;
};
}  // namespace web
