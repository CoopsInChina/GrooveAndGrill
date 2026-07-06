#include "globals.h"
#include "app_config.h"
#include "display.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "globals";

// ---- State storage ------------------------------------------------

int  g_brightness        = DEFAULT_BRIGHTNESS;
int  g_brightness_dimmed = DIM_BRIGHTNESS;
bool g_dim_enabled       = DEFAULT_DIM_ENABLED;
int  g_autodim_sec       = DEFAULT_AUTODIM_SEC;
bool g_screen_dimmed     = false;
uint32_t g_last_touch_ms = 0;

bool g_screensaver_enabled = DEFAULT_SCREENSAVER_ENABLED;
int  g_screensaver_sec     = DEFAULT_SCREENSAVER_SEC;

static bool s_screensaver_triggered = false;

SemaphoreHandle_t        g_network_mutex      = NULL;
volatile uint32_t        g_last_network_end_ms = 0;
volatile bool            g_ntp_synced         = false;

char g_api_server[64] = {0};

// ---- Helpers --------------------------------------------------------

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ---- Lifecycle ------------------------------------------------------

void globals_init(void)
{
    g_network_mutex = xSemaphoreCreateMutex();
    if (!g_network_mutex) {
        ESP_LOGE(TAG, "Failed to create network mutex");
    }

    g_last_touch_ms = now_ms();

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "No saved globals — using defaults");
        return;
    }

    int32_t v;
    if (nvs_get_i32(nvs, NVS_KEY_BRIGHTNESS, &v) == ESP_OK)
        g_brightness = (int)v;
    if (nvs_get_i32(nvs, NVS_KEY_AUTODIM_SEC, &v) == ESP_OK)
        g_autodim_sec = (int)v;
    if (nvs_get_i32(nvs, NVS_KEY_DIM_ENABLED, &v) == ESP_OK)
        g_dim_enabled = (bool)v;
    if (nvs_get_i32(nvs, NVS_KEY_SCREENSAVER_EN, &v) == ESP_OK)
        g_screensaver_enabled = (bool)v;
    if (nvs_get_i32(nvs, NVS_KEY_SCREENSAVER_SEC, &v) == ESP_OK)
        g_screensaver_sec = (int)v;

    size_t len = sizeof(g_api_server);
    nvs_get_str(nvs, NVS_KEY_API_SERVER, g_api_server, &len);

    nvs_close(nvs);

    ESP_LOGI(TAG, "Loaded: brightness=%d autodim=%ds api=%s",
             g_brightness, g_autodim_sec, g_api_server[0] ? g_api_server : "(none)");
}

void globals_save_display(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_i32(nvs, NVS_KEY_BRIGHTNESS,      g_brightness);
    nvs_set_i32(nvs, NVS_KEY_AUTODIM_SEC,     g_autodim_sec);
    nvs_set_i32(nvs, NVS_KEY_DIM_ENABLED,     (int32_t)g_dim_enabled);
    nvs_set_i32(nvs, NVS_KEY_SCREENSAVER_EN,  (int32_t)g_screensaver_enabled);
    nvs_set_i32(nvs, NVS_KEY_SCREENSAVER_SEC, (int32_t)g_screensaver_sec);
    nvs_commit(nvs);
    nvs_close(nvs);
}

void globals_save_api_server(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_str(nvs, NVS_KEY_API_SERVER, g_api_server);
    nvs_commit(nvs);
    nvs_close(nvs);
}

// ---- Touch / autodim ------------------------------------------------

void globals_touch_activity(void)
{
    g_last_touch_ms = now_ms();
    s_screensaver_triggered = false;
    if (g_screen_dimmed) {
        g_screen_dimmed = false;
        display_set_brightness(g_brightness);
    }
}

static void brightness_anim_cb(void *var, int32_t val)
{
    (void)var;
    display_set_brightness((int)val);
}

bool globals_screensaver_should_trigger(void)
{
    if (!g_screensaver_enabled) return false;
    if (s_screensaver_triggered) return false;
    uint32_t elapsed = now_ms() - g_last_touch_ms;
    if (elapsed < (uint32_t)(g_screensaver_sec * 1000)) return false;
    s_screensaver_triggered = true;
    return true;
}

void globals_check_autodim(void)
{
    if (!g_dim_enabled || g_autodim_sec == 0) return;
    if (g_screen_dimmed) return;

    uint32_t elapsed = now_ms() - g_last_touch_ms;
    if (elapsed < (uint32_t)(g_autodim_sec * 1000)) return;

    g_screen_dimmed = true;
    ESP_LOGI(TAG, "Autodim: fading to %d%%", g_brightness_dimmed);

    // lv_anim_start() touches LVGL internals — must hold the display lock.
    // Called from timer task context, so use a short timeout rather than blocking.
    if (display_lock(100)) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, NULL);
        lv_anim_set_values(&a, g_brightness, g_brightness_dimmed);
        lv_anim_set_time(&a, 1000);
        lv_anim_set_exec_cb(&a, brightness_anim_cb);
        lv_anim_start(&a);
        display_unlock();
    } else {
        display_set_brightness(g_brightness_dimmed);
    }
}
