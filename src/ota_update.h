#pragma once

#include <stdbool.h>

// ============================================================================
// OTA update client
// ----------------------------------------------------------------------------
// Manually triggered from the Settings > OTA page: check version.json on
// GitHub Pages (published only from the `release` branch — see README), then
// on request, stream firmware.bin into the inactive OTA slot via
// esp_https_ota() and hand off to the reboot-countdown screen.
//
// Network calls run in their own short-lived tasks so callers (LVGL) never
// block; poll ota_get_status() from a UI timer instead.
// ============================================================================

typedef enum {
    OTA_IDLE = 0,
    OTA_CHECKING,
    OTA_UP_TO_DATE,
    OTA_UPDATE_AVAILABLE,
    OTA_CHECK_FAILED,
    OTA_UPDATING,
    OTA_DONE_OK,
    OTA_DONE_FAIL,
} ota_state_t;

typedef struct {
    ota_state_t state;
    char        latest_version[16];   // valid once state >= OTA_UP_TO_DATE
    int         bytes_read;
    int         image_size;           // 0 if the server didn't send a size
    char        error[64];
} ota_status_t;

void ota_check_async(void);
void ota_start_async(void);
void ota_get_status(ota_status_t *out);

// Call once at boot after things look healthy (e.g. once the menu screen is
// up) to confirm this image and cancel the rollback timer — see
// CONFIG_APP_ROLLBACK_ENABLE in sdkconfig.defaults.
void ota_mark_app_valid(void);
