#pragma once

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Direct BLE meat-probe support (no BBQ Box)
// ============================================================================
// Scans for wireless "BBQ" thermometers (ODM CXL-...) and holds up to
// MAX_DIRECT_PROBES concurrent GATT connections, each streaming temperature via
// a NOTIFY characteristic (payload: [1 prefix byte][ASCII decimal temp]).
// Used when the source mode is BBQ_SRC_PROBE. Protocol reverse-engineered in
// docs/bbq_ble_probe_integration.md.
//
// Needs the BLE CENTRAL role; the controller is sized for MAX_DIRECT_PROBES
// connections (see sdkconfig CONFIG_BT_NIMBLE_MAX_CONNECTIONS / BLE_MAX_ACT).

#define MAX_DIRECT_PROBES 2

// Start NimBLE (central) and begin scanning. Reconnects automatically as links
// drop and free up. Call once, after WiFi.
void ble_probe_init(void);

// Number of probes currently connected AND reporting within the freshness
// window.
int  ble_probe_count(void);

// The i-th (0..count-1) connected+fresh probe: fills `id` (a stable tag = low
// byte of the probe's address, used as the sensor hw_id) and `temp_c`.
// Returns false if `i` is out of range.
bool ble_probe_at(int i, uint8_t *id, float *temp_c);

// True if at least one probe link is up (drives the on-screen BT indicator).
bool ble_probe_any(void);
