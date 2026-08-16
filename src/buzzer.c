#include "buzzer.h"
#include "tca9554.h"
#include "esp_timer.h"
#include <stdbool.h>

// Simple on/off buzzer (no PWM/tone control) toggled at ~1.7 Hz for a
// beep-beep alarm pattern. tca9554_set_pin() does a blocking I2C write, so
// this runs from an esp_timer callback (its own Timer Service task) rather
// than an LVGL timer, to avoid ever stalling the UI on I2C.
#define BUZZER_PIN         8
#define TOGGLE_PERIOD_US   300000   // 300ms -> ~1.7Hz on/off

static esp_timer_handle_t s_timer;
static volatile bool      s_alarm_active;
static bool                s_pin_on;

static void toggle_cb(void *arg)
{
    (void)arg;
    if (!s_alarm_active) {
        if (s_pin_on) { tca9554_set_pin(BUZZER_PIN, false); s_pin_on = false; }
        return;
    }
    s_pin_on = !s_pin_on;
    tca9554_set_pin(BUZZER_PIN, s_pin_on);
}

void buzzer_init(void)
{
    const esp_timer_create_args_t targs = { .callback = toggle_cb, .name = "buzzer" };
    if (esp_timer_create(&targs, &s_timer) == ESP_OK)
        esp_timer_start_periodic(s_timer, TOGGLE_PERIOD_US);
}

void buzzer_set_alarm(bool on)
{
    s_alarm_active = on;
    if (!on && s_pin_on) {
        tca9554_set_pin(BUZZER_PIN, false);   // silence immediately, don't wait for the next tick
        s_pin_on = false;
    }
}
