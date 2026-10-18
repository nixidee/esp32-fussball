// Private portal-DNS owner inside the network component.
#pragma once
#include "esp_err.h"

namespace network {
// Network initialization owns task admission; retry only until this succeeds.
esp_err_t startPortalDns();
// Runtime socket failures/recovery, relayed by the network status owner.
esp_err_t portalDnsError();
}  // namespace network
