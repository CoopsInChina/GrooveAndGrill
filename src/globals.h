#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ============================================================
// App-wide runtime state — persisted in NVS, shared across tasks
// ============================================================

// Display / autodim
extern int  g_brightness;        // current backlight % (0-100)
extern int  g_brightness_dimmed; // backlight % when autodimmed
extern bool g_dim_enabled;       // false = autodim disabled
extern int  g_autodim_sec;       // seconds before dim (10-120)
extern bool g_screen_dimmed;
extern uint32_t g_last_touch_ms;

// Screensaver
extern bool g_screensaver_enabled;
extern int  g_screensaver_sec;   // seconds before screensaver (60-1800)

// Network serialisation — ALL HTTP/UDP ops must hold this
extern SemaphoreHandle_t g_network_mutex;

// SDIO/network timing — for cooldown guards
extern volatile uint32_t g_last_network_end_ms;

// NTP
extern volatile bool g_ntp_synced;

// node-sonos-http-api server address (stored in NVS)
extern char g_api_server[64];   // e.g. "192.168.1.100:5005"

// ---- Lifecycle ------------------------------------------------
// Load persisted values from NVS, create mutex. Call at boot before tasks start.
void globals_init(void);

// Persist display settings to NVS
void globals_save_display(void);

// Persist API server address to NVS
void globals_save_api_server(void);

// ---- Touch tracking -------------------------------------------
// Call from LVGL touch event callback to reset autodim timer
void globals_touch_activity(void);

// Call from main loop (or timer) to trigger autodim
void globals_check_autodim(void);

// Returns true once per inactivity cycle when screensaver should launch.
// Caller must navigate to SCREEN_WIDGETS under display_lock.
bool globals_screensaver_should_trigger(void);
