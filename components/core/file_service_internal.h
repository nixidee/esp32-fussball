// Internal access for the device test of the file service (file_test.cpp).

#pragma once

#include "esp_err.h"
#include "esp_partition.h"

namespace files::internal {

// The partition found by init(); nullptr if there is none.
const esp_partition_t* partition();

// Unmounts; the state becomes kUnavailable until format() or the next boot.
esp_err_t unmount();

}  // namespace files::internal
