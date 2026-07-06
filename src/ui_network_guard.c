#include "ui_network_guard.h"
#include "globals.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "net_guard";

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

bool net_pre_wait(const char* tag, uint32_t flags)
{
    // General cooldown: minimum gap between consecutive HTTP operations.
    // Prevents TCP SYN-ACK from a new request colliding with the FIN-ACK from
    // the previous request on the same WiFi RX path.
    if (flags & NET_WAIT_GENERAL) {
        if (g_last_network_end_ms > 0) {
            uint32_t elapsed = now_ms() - g_last_network_end_ms;
            if (elapsed < SDIO_GENERAL_COOLDOWN_MS) {
                uint32_t wait = SDIO_GENERAL_COOLDOWN_MS - elapsed;
                ESP_LOGD(TAG, "[%s] General cooldown: waiting %lums", tag, (unsigned long)wait);
                vTaskDelay(pdMS_TO_TICKS(wait));
            }
        }
    }

    // Large-download cooldown: after fetching album art (several KB), allow
    // extra drain time before the next HTTP op.
    if (flags & NET_WAIT_LARGE) {
        if (g_last_network_end_ms > 0) {
            uint32_t elapsed = now_ms() - g_last_network_end_ms;
            if (elapsed < 1000) {
                uint32_t wait = 1000 - elapsed;
                ESP_LOGD(TAG, "[%s] Post-download cooldown: waiting %lums", tag, (unsigned long)wait);
                vTaskDelay(pdMS_TO_TICKS(wait));
            }
        }
    }

    return true;
}
