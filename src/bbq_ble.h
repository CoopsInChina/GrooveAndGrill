#pragma once

#include <stdbool.h>
#include <stdint.h>

// Passive BLE receiver for the ESP-BLE-BBQ-Box gateway.
//
// The box is the household's single BLE gateway: it reads up to 4 wired
// thermocouples (MAX31855) AND connects to wireless BBQ probes as a central,
// then broadcasts the lot in one connectionless legacy advertisement
// (company 0xFFFF, version 0x02). We just observe (passive scan) — no pairing,
// no connection, so adding probes costs the display nothing. See that
// project's docs/ble_protocol.md for the payload layout.

#define BBQ_BLE_CHANNELS   4   // wired thermocouples
#define BBQ_BLE_MAX_PROBES 5   // wireless probes carried in a legacy advert

// Start NimBLE (observer role) and begin scanning. Call once, after WiFi.
void bbq_ble_init(void);

// Latest temperature for thermocouple channel `ch` (0..3).
// Returns true and fills `temp_c` only when the box has been heard recently
// AND that channel is reporting a valid reading (not faulted/absent).
bool bbq_ble_channel(int ch, float *temp_c);

// Number of wireless probes in the box's most recent advertisement.
// 0 if the box hasn't been heard recently or is carrying no probes.
int bbq_ble_probe_count(void);

// Wireless probe at `slot` (0..count-1) from the last advertisement.
// Fills `id` (stable 1-byte tag) and `temp_c`. Returns false if the box is
// stale or `slot` is out of range.
bool bbq_ble_probe(int slot, uint8_t *id, float *temp_c);

// Milliseconds since the box was last heard (UINT32_MAX if never heard).
uint32_t bbq_ble_age_ms(void);

// True if the box has been heard within the freshness window.
bool bbq_ble_present(void);
