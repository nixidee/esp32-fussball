// The worker's settings values. No Wi-Fi/password/UI/policy snapshot survives
// in the provider stack; source routes and eligibility have separate borrows.
#pragma once
#include "settings_model.h"
namespace settings {
struct ProviderSettings {
  Bytes<cfg::kApiFootballKeyMaxBytes> api_football_key;
  Bytes<cfg::kFootballDataKeyMaxBytes> football_data_key;
  bool espn_opt_in = cfg::kEspnOptInDefault, demo = cfg::kDemoDefault;
  ProviderPlans providers = providerPlansDefault();
  RequestLimit global_limit;
};
inline void projectProviderSettings(const Model& model,
                                    ProviderSettings& out) noexcept {
  out.api_football_key = model.api_football_key;
  out.football_data_key = model.football_data_key;
  out.espn_opt_in = model.espn_opt_in;
  out.demo = model.demo;
  out.providers = model.providers;
  out.global_limit = model.global_limit;
}
namespace detail {
template <class Values>
bool providerUsableFields(cfg::Provider provider,
                          const Values& settings) noexcept {
  switch (provider) {
    case cfg::Provider::kEspn: return settings.espn_opt_in;
    case cfg::Provider::kApiFootball: return !settings.api_football_key.empty();
    case cfg::Provider::kFootballData:
      return !settings.football_data_key.empty();
    default: return true;
  }
}
}  // namespace detail
inline bool providerUsable(cfg::Provider provider,
                           const ProviderSettings& settings) noexcept {
  return detail::providerUsableFields(provider, settings);
}
}  // namespace settings
