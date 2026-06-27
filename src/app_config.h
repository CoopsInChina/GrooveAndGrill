#pragma once

// ============================================================
// Application-level configuration
// All CAN signal parameters MUST be updated to match your
// vehicle's CAN database before the firmware will work correctly.
// ============================================================

// ---- CAN bus speed ----
// Common values: 125000, 250000, 500000, 1000000
#define CAN_BITRATE_BPS         500000

// ---- Vehicle speed signal ----
// Frame containing the wheel/vehicle speed value.
//
// Conversion: speed_mph = ((raw * SPEED_SCALE) + SPEED_OFFSET) * KMH_TO_MPH
// Set KMH_TO_MPH = 1.0f if the raw value is already in mph.
//
#define SPEED_CAN_ID            0x4B0U  // 11-bit standard frame ID
#define SPEED_IS_EXTENDED       false   // true = 29-bit extended frame
#define SPEED_BYTE_OFFSET       0U      // first byte of the speed value
#define SPEED_BYTE_COUNT        2U      // 2 = big-endian uint16, 1 = uint8
#define SPEED_SCALE             0.01f   // raw -> km/h  (e.g. 10000 raw = 100 km/h)
#define SPEED_OFFSET            0.0f    // km/h additive offset after scaling
#define KMH_TO_MPH              0.621371f

// Maximum speed the gauge arc covers (mph)
#define SPEED_MAX_DISPLAY       199.0f

// ---- Speed display calibration ----
// Percentage offset applied after CAN scaling: display = raw_mph * (1 + pct/100)
// Persisted in NVS; adjusted via the WiFi calibration server.
#define CAL_MAX_OFFSET_PCT      25.0f   // clamp range: ±25%

// ---- ComfortEnable signal ----
// A single bit inside a CAN frame that gates the screen on/off.
// Screen ON  when the bit transitions 0->1.
// Screen OFF when the bit transitions 1->0.
//
#define COMFORT_CAN_ID          0x3B0U
#define COMFORT_IS_EXTENDED     false
#define COMFORT_BYTE_OFFSET     0U
#define COMFORT_BIT_MASK        0x01U   // isolate the ComfortEnable bit
