#pragma once

#include <stdbool.h>
#include <stdint.h>

// ============================================================
// Network guard — serialises concurrent WiFi/HTTP operations.
// On ESP32-S3 (internal WiFi, not SDIO) the cooldown values
// are conservative but not crash-critical like the P4 SDIO case.
// ============================================================

#define SDIO_GENERAL_COOLDOWN_MS    200     // minimum gap between any two HTTP ops
#define NETWORK_MUTEX_TIMEOUT_MS    3000    // max wait for network_mutex

// Wait flags for net_pre_wait()
#define NET_WAIT_GENERAL    (1 << 0)    // always include: 200ms general cooldown
#define NET_WAIT_LARGE      (1 << 1)    // after large downloads (art etc): 1000ms

// Wait for network cooldowns then return. Call before acquiring g_network_mutex.
// tag: short string for log output.
// Returns false if waiting was somehow interrupted (future: abort flag).
bool net_pre_wait(const char* tag, uint32_t flags);
