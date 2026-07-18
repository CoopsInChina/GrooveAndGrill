#include "bbq_controller.h"
#include "bbq_ble.h"
#include "esp_timer.h"
#include <string.h>

static bbq_grill_t s_grills[MAX_GRILLS];
static int         s_count = 0;

static esp_timer_handle_t s_ble_timer;

// Map the BBQ Box's 4 thermocouples onto grills: each grill takes a pair —
// grill 0 = TC0 (grill) + TC1 (meat), grill 1 = TC2 + TC3. Only runs while the
// box is being heard; when it's absent the mock/UI state is left untouched so
// on-bench UI testing still works. (v1 fixed mapping — make configurable later.)
static void poll_ble(void)
{
    if (!bbq_ble_present()) return;

    for (int idx = 0; idx < s_count; idx++) {
        int tc_grill = idx * 2;
        int tc_meat  = idx * 2 + 1;
        if (tc_grill >= BBQ_BLE_CHANNELS) continue;   // no box channels for this grill

        float gt, mt;
        bool g_ok = bbq_ble_channel(tc_grill, &gt);
        bool m_ok = bbq_ble_channel(tc_meat,  &mt);

        bbq_grill_t *g = &s_grills[idx];
        if (g_ok) g->grill_temp_c = gt;
        if (m_ok) g->meat_temp_c  = mt;

        if (g_ok || m_ok)
            g->probe_state = PROBE_CONNECTED;
        else if (g->probe_state == PROBE_CONNECTED)
            g->probe_state = PROBE_DISCONNECTED;      // was live, lost the channel
    }
}

static void ble_timer_cb(void *arg) { (void)arg; poll_ble(); }

void bbq_controller_init(void)
{
    memset(s_grills, 0, sizeof(s_grills));
    s_count = 1;   // must always have at least 1 grill

    const esp_timer_create_args_t targs = {
        .callback = ble_timer_cb,
        .name     = "bbq_ble_poll",
    };
    if (esp_timer_create(&targs, &s_ble_timer) == ESP_OK)
        esp_timer_start_periodic(s_ble_timer, 1000000);   // 1 Hz
}

int bbq_grill_count(void)
{
    return s_count;
}

bool bbq_add_grill(void)
{
    if (s_count >= MAX_GRILLS) return false;
    s_grills[s_count] = (bbq_grill_t){0};
    s_count++;
    return true;
}

const bbq_grill_t *bbq_get_grill(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_grills[idx];
}

void bbq_mock_connect_probe(int idx)
{
    if (idx < 0 || idx >= s_count) return;
    s_grills[idx].probe_state = PROBE_CONNECTED;
    s_grills[idx].meat_temp_c  = 25.0f;
    s_grills[idx].grill_temp_c = 350.0f;
}

void bbq_mock_toggle_probe(int idx)
{
    if (idx < 0 || idx >= s_count) return;
    bbq_grill_t *g = &s_grills[idx];
    if (g->probe_state == PROBE_CONNECTED) {
        g->probe_state = PROBE_DISCONNECTED;
    } else if (g->probe_state == PROBE_DISCONNECTED) {
        g->probe_state = PROBE_CONNECTED;
        g->meat_temp_c  = 25.0f;
        g->grill_temp_c = 350.0f;
    }
}

void bbq_set_targets(int idx, int grill_target_c, int meat_target_c, meat_kind_t kind)
{
    if (idx < 0 || idx >= s_count) return;
    s_grills[idx].configured      = true;
    s_grills[idx].meat_kind        = kind;
    s_grills[idx].grill_target_c  = grill_target_c;
    s_grills[idx].meat_target_c   = meat_target_c;
}

void bbq_set_grill_target(int idx, int grill_target_c)
{
    if (idx < 0 || idx >= s_count) return;
    s_grills[idx].grill_target_c = grill_target_c;
}
