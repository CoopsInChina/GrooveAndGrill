#include "tca9554.h"
#include "display.h"
#include "cst820.h"
#include "globals.h"
#include "wifi_manager.h"
#include "sonos_controller.h"
#include "weather.h"
#include "ui_common.h"
#include "ui_boot.h"
#include "ui_art.h"
#include "web_server.h"
#include "app_config.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <stdio.h>

static const char *TAG = "main";

// ---- Autodim timer (fires every 3s, same cadence as SonosESP) ------

static void ntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    g_ntp_synced = true;
    ESP_LOGI(TAG, "NTP synced");
}

static void autodim_timer_cb(TimerHandle_t t)
{
    (void)t;
    globals_check_autodim();
    if (globals_screensaver_should_trigger()) {
        if (display_lock(100)) {
            ui_navigate_to(SCREEN_WIDGETS);
            display_unlock();
        }
    }
}

// ---- Boot ----------------------------------------------------------

void app_main(void)
{
    // NVS — required by wifi_manager, globals, and settings persistence
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Globals (brightness, autodim, network mutex) — before any tasks
    globals_init();

    // ── EARLY WiFi BEGIN (SonosESP pattern) ──────────────────────────
    // Start connecting before display init so the ~2-3s LVGL startup
    // overlaps with the radio association. This shaves 2-3s off boot time.
    wifi_manager_init();
    bool has_creds = wifi_manager_begin_connect();

    // I2C expander — must come before display_init
    ESP_ERROR_CHECK(tca9554_init());

    // Display + LVGL task — runs while WiFi connects in background
    ESP_ERROR_CHECK(display_init());
    display_set_backlight(true);
    display_set_brightness(g_brightness);

    // Touch + boot screen — must be inside display_lock
    if (display_lock(5000)) {
        cst820_init();
        ui_boot_show();
        display_unlock();
    }

    // ── Memory diagnostics (matches SonosESP boot log) ───────────────
    ESP_LOGI(TAG, "=== MEMORY MAP (post-display, pre-WiFi-wait) ===");
    ESP_LOGI(TAG, "  DRAM free:  %u bytes", (unsigned)heap_caps_get_free_size(0x00000004));
    ESP_LOGI(TAG, "  PSRAM free: %u bytes", (unsigned)heap_caps_get_free_size(0x00000200));

    // ── Wait for WiFi ─────────────────────────────────────────────────
    if (display_lock(500)) {
        ui_boot_set_status(has_creds ? "WiFi: Connecting..." : "WiFi: No credentials", 20);
        display_unlock();
    }

    bool wifi_ok = false;
    if (has_creds) {
        // wait_connect implements 3-retry loop (per SonosESP)
        wifi_ok = wifi_manager_wait_connect(WIFI_CONNECT_TIMEOUT_MS);
    }

    if (display_lock(500)) {
        char msg[80];
        if (wifi_ok)
            snprintf(msg, sizeof(msg), "WiFi: %s (%s)",
                     wifi_manager_ssid(), wifi_manager_ip());
        else
            snprintf(msg, sizeof(msg), "WiFi: Not connected");
        ui_boot_set_status(msg, 40);
        ui_boot_set_icon(BOOT_ICON_WIFI, wifi_ok ? BOOT_STATE_OK : BOOT_STATE_FAIL);
        display_unlock();
    }
    ESP_LOGI(TAG, "WiFi: %s", wifi_ok ? wifi_manager_ssid() : "not connected");

    // ── Sonos discovery ───────────────────────────────────────────────
    if (display_lock(500)) {
        ui_boot_set_status("Sonos: Searching...", 60);
        display_unlock();
    }

    sonos_controller_init();
    bool sonos_ok = false;
    if (wifi_ok) {
        sonos_ok = sonos_controller_discover(SONOS_DISCOVERY_TIMEOUT_MS);
    }

    if (display_lock(500)) {
        char msg[64];
        if (sonos_ok)
            snprintf(msg, sizeof(msg), "Sonos: %s", sonos_active_speaker_name());
        else
            snprintf(msg, sizeof(msg), "Sonos: Not found");
        ui_boot_set_status(msg, 80);
        ui_boot_set_icon(BOOT_ICON_SONOS, sonos_ok ? BOOT_STATE_OK : BOOT_STATE_FAIL);
        display_unlock();
    }
    ESP_LOGI(TAG, "Sonos: %s", sonos_ok ? sonos_active_speaker_name() : "not found");

    // ── node-sonos-http-api discovery ────────────────────────────────
    // Checks NVS cache; if unreachable starts background /24 subnet scan.
    if (display_lock(300)) {
        ui_boot_set_status("API: Checking...", 88);
        display_unlock();
    }

    bool api_ok = false;
    if (wifi_ok) {
        api_ok = sonos_api_server_init(wifi_manager_ip());
    }

    // ── Setup web server (port 80) ────────────────────────────────────
    bool srv_ok = false;
    if (wifi_ok) {
        srv_ok = web_server_start();
    }

    if (display_lock(300)) {
        const char *status = api_ok ? "API: Found" : "API: Scanning...";
        ui_boot_set_status(status, 100);
        ui_boot_set_icon(BOOT_ICON_SERVER, srv_ok ? BOOT_STATE_OK : BOOT_STATE_FAIL);
        display_unlock();
    }
    ESP_LOGI(TAG, "API server: %s", api_ok ? g_api_server : "scanning in background");
    ESP_LOGI(TAG, "Setup server: %s", srv_ok ? "running" : "failed");

    // Hold boot screen so user can read the status dots
    vTaskDelay(pdMS_TO_TICKS(2000));

    // ── Navigate to start screen ──────────────────────────────────────
    if (display_lock(1000)) {
        ui_navigate_to(sonos_ok ? SCREEN_SONOS : SCREEN_MENU);
        display_unlock();
    }

    // ── NTP — start as soon as WiFi is up ─────────────────────────────
    if (wifi_ok) {
        if (esp_sntp_enabled()) esp_sntp_stop();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_set_time_sync_notification_cb(ntp_sync_cb);
        esp_sntp_init();
    }

    // ── Background tasks ──────────────────────────────────────────────
    ui_art_init();
    weather_init();
    sonos_controller_start_polling();
    wifi_manager_start_monitor();

    // Autodim check timer — fires every 3s (identical to SonosESP main loop cadence)
    TimerHandle_t autodim_timer = xTimerCreate("autodim", pdMS_TO_TICKS(3000),
                                               pdTRUE, NULL, autodim_timer_cb);
    if (autodim_timer) xTimerStart(autodim_timer, 0);

    ESP_LOGI(TAG, "Music and Meat v%s running — heap: %lu free",
             FIRMWARE_VERSION, (unsigned long)esp_get_free_heap_size());
}
