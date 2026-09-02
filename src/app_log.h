#pragma once

// ============================================================
// Project-wide logging: fixed [LEVEL] (TAG) (time) "message" format,
// plus a flash-backed rolling history of WARN/ERROR lines that survives
// reboots/power loss — retrievable from the web server without a UART
// cable attached at the time something went wrong.
//
// Four levels only:
//   LOGI — routine status, the bulk of output.
//   LOGW — a possible failure (e.g. can't connect to WiFi).
//   LOGE — a real problem (e.g. a service failed to start, out of memory).
//   LOGD — temporary, placed to chase a specific fault; off by default,
//          flip app_log_set_debug_enabled(true) while actively debugging.
//
// This intentionally does NOT go through ESP_LOGx/esp_log — that's what
// gives us full control over the format and a single chokepoint to also
// capture WARN/ERROR to flash. Vendored/IDF-internal components (WiFi,
// NimBLE, LVGL, the boot ROM) still log in ESP-IDF's own format; only this
// project's own ~170 call sites go through here.
// ============================================================

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_LOG_INFO = 0,
    APP_LOG_WARN,
    APP_LOG_ERROR,
    APP_LOG_DEBUG,
} app_log_level_t;

// Call once at boot, before any LOGx call that should reach the flash
// ring buffer — scans the fault_log partition to resume numbering after
// a reboot. Safe to call LOGx before this (they just won't be captured
// to flash yet); UART output always works regardless.
void app_log_init(void);

// LOGD is a no-op unless this has been turned on — off by default.
void app_log_set_debug_enabled(bool enabled);
bool app_log_debug_enabled(void);

// Formats and prints one line; WARN/ERROR are also appended to the flash
// ring buffer. Use the LOGx macros below rather than calling this directly.
void app_log_emit(app_log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOGI(tag, fmt, ...) app_log_emit(APP_LOG_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) app_log_emit(APP_LOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOGE(tag, fmt, ...) app_log_emit(APP_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define LOGD(tag, fmt, ...) app_log_emit(APP_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)

// ---- Fault-log retrieval (web server) ----------------------------------

typedef struct {
    uint32_t timestamp_ms;   // esp_log_timestamp() value at the time
    app_log_level_t level;   // always WARN or ERROR
    char tag[16];
    char msg[96];
} app_log_fault_record_t;

// Reads up to max_out records, oldest first, into out. Returns the number
// written. Safe to call from the web server task.
int app_log_read_faults(app_log_fault_record_t *out, int max_out);

// Erases the whole fault-log partition (e.g. a "clear log" button).
void app_log_clear_faults(void);
