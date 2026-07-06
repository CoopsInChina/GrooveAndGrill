#pragma once

#include <stdbool.h>

// Start the setup HTTP server on port 80.
// Returns true on success. Safe to call more than once (idempotent).
bool web_server_start(void);

bool web_server_running(void);
