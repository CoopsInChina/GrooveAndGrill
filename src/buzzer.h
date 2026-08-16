#pragma once

#include <stdbool.h>

// Onboard buzzer, wired via the TCA9554 IO expander (pin 8) — see the
// "Buzzer" pads on the board silkscreen. Call after tca9554_init().
void buzzer_init(void);

// Starts/stops a periodic beep while an alarm condition is active (a sensor
// that was live has stopped reporting). Idempotent — safe to call every poll
// tick with the same value.
void buzzer_set_alarm(bool on);
