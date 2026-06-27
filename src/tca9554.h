#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Initialise I2C master and the TCA9554PWR expander.
// All 8 pins are configured as outputs, initially high.
esp_err_t tca9554_init(void);

// Set a single pin (1-based, 1..8) to high (true) or low (false)
// without disturbing the other pins.
esp_err_t tca9554_set_pin(uint8_t pin, bool level);
