#pragma once

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Direct BLE meat-probe support
// ============================================================================
// Reads a wireless BLE BBQ thermometer (advertised name "BBQ", ODM serial
// CXL-...). Unlike the BBQ Box (connectionless advertising), this device is
// GATT connection-based: scan -> connect -> subscribe to a NOTIFY
// characteristic that streams ASCII temperature. Protocol reverse-engineered
// in docs/bbq_ble_probe_integration.md.
//
//   Notification payload: [1 prefix byte][ASCII decimal temp], e.g. "@31.4".
//   Single temperature channel. Prefix byte meaning is unresolved — kept raw.
//
// Architecture note: this needs the BLE CENTRAL role (active connection),
// whereas feature/BLEBoxIntegration only needs OBSERVER (passive scan). On
// merge, one NimBLE stack must serve both: passively read box adverts AND
// maintain this probe connection.

typedef struct {
    bool     connected;   // GATT link up and subscribed
    float    temp_c;      // last notified temperature
    uint8_t  prefix;      // raw prefix byte (meaning UNRESOLVED — see doc)
    int8_t   rssi;        // from the advertisement at connect time
    uint32_t last_ms;     // last notification time (esp_timer ms)
} ble_probe_t;

// Start NimBLE (central) and begin scanning for the thermometer. Reconnects
// automatically if the link drops. Call once, after WiFi.
void ble_probe_init(void);

// Latest temperature. Returns true (and fills temp_c) only while connected and
// a notification has arrived within the freshness window.
bool ble_probe_get(float *temp_c);

bool     ble_probe_connected(void);
uint32_t ble_probe_age_ms(void);   // ms since last notification (UINT32_MAX if none)
