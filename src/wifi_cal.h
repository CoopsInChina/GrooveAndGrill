#pragma once

#include "esp_err.h"

// Start the WiFi SoftAP and HTTP calibration server.
// Connect a phone/laptop to the AP defined in app_config.h,
// then open http://192.168.4.1 in a browser to adjust the offset.
esp_err_t wifi_cal_server_start(void);
