#pragma once

#include <stdbool.h>
#include <stdint.h>

// ============================================================
// WiFi manager — STA connection + AP captive portal for setup
// ============================================================

// Step 1: Initialise WiFi stack. Call before display_init() for early begin.
void wifi_manager_init(void);

// Step 2: Load saved credentials and start connecting in the background.
// Returns false immediately if no credentials are saved.
// The connection runs asynchronously — call wifi_manager_wait_connect() to block.
bool wifi_manager_begin_connect(void);

// Step 3: Wait for the connection started by begin_connect() to succeed or fail.
// timeout_ms: max wait. Returns true on success.
bool wifi_manager_wait_connect(uint32_t timeout_ms);

// Convenience: begin + wait in one call (used when display is already up).
bool wifi_manager_connect(uint32_t timeout_ms);

// Start background connection monitor (reconnects on drop, calls NTP on reconnect)
void wifi_manager_start_monitor(void);

// Start NTP time sync. Called automatically after connect; can also be called manually.
void wifi_manager_start_ntp(void);

// Start the setup AP + captive portal HTTP server.
// QR code URL: http://192.168.4.1
void wifi_manager_start_setup_ap(void);

// Stop the setup AP
void wifi_manager_stop_setup_ap(void);

// True if currently connected to a STA network
bool wifi_manager_is_connected(void);

// SSID of current connection ("" if not connected)
const char *wifi_manager_ssid(void);

// IP address string of current connection ("" if not connected)
const char *wifi_manager_ip(void);

// Save credentials to NVS (called by captive portal handler)
void wifi_manager_save_credentials(const char *ssid, const char *pass);

// True if the setup AP is currently active
bool wifi_manager_ap_active(void);
