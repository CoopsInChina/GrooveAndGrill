#pragma once

#include <stdbool.h>
#include <stdint.h>

// Passive BLE receiver for the ESP-BLE-BBQ-Box satellite.
//
// The box broadcasts connectionless legacy advertisements (name "BBQBox",
// manufacturer data: company 0xFFFF, version 0x01, fault byte, then 4x uint16
// LE temperatures in 0.1 C). See that project's docs/ble_protocol.md. We just
// observe (passive scan) — no pairing, no connection.

#define BBQ_BLE_CHANNELS   4

// Start NimBLE (observer role) and begin scanning. Call once, after WiFi.
void bbq_ble_init(void);

// Latest temperature for thermocouple channel `ch` (0..3).
// Returns true and fills `temp_c` only when the box has been heard recently
// AND that channel is reporting a valid reading (not faulted/absent).
bool bbq_ble_channel(int ch, float *temp_c);

// Milliseconds since the box was last heard (UINT32_MAX if never heard).
uint32_t bbq_ble_age_ms(void);

// True if the box has been heard within the freshness window.
bool bbq_ble_present(void);
