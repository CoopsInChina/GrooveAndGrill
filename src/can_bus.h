#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Called from the CAN RX task whenever a new speed sample arrives.
typedef void (*speed_update_cb_t)(float speed_mph);

// Called from the CAN RX task whenever ComfortEnable transitions.
typedef void (*comfort_enable_cb_t)(bool enabled);

// Initialise the TWAI driver in listen-only mode and start the RX task.
esp_err_t can_bus_init(speed_update_cb_t speed_cb, comfort_enable_cb_t comfort_cb);
