// P1.8 integration budget probe (diagnostic). Compiled only in the env
// xiao_esp32c6_gc9a01_budget and removed after the measurements (OQ-24 B).
//
// Starts one task that brings up WiFi (station), SNTP, HTTPS requests to
// OpenLigaDB with filtered ArduinoJson parsing and shows the result on the
// display. Every phase logs its time, heap and stack use.

#pragma once

namespace probe {

// Call once after display::init(). Returns immediately.
void start();

}  // namespace probe
