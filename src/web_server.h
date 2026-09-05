#pragma once

#include <stdbool.h>

// Start the setup HTTP server on port 80.
// Returns true on success. Safe to call more than once (idempotent).
bool web_server_start(void);

// Stops it — needed while the WiFi setup captive portal is active, since
// both servers default to port 80 and only one can bind it at a time.
void web_server_stop(void);

bool web_server_running(void);
