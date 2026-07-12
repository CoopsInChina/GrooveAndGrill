#pragma once

#include "app_config.h"
#include <stdbool.h>

#define MAX_GRILLS  MAX_BBQ_PROBES

typedef enum {
    PROBE_NONE = 0,        // never added to this grill slot
    PROBE_CONNECTED,
    PROBE_DISCONNECTED,    // was connected, lost signal
} probe_state_t;

// Identifies which meat-icon to show — kept separate from the MEAT_TYPES
// table in meat_temps.h (which only covers meats with doneness levels;
// chicken has a single food-safety target and isn't in that table).
typedef enum {
    MEAT_KIND_NONE = 0,
    MEAT_KIND_CHICKEN,
    MEAT_KIND_LAMB,
    MEAT_KIND_PORK,
    MEAT_KIND_BEEF,
} meat_kind_t;

typedef struct {
    bool          configured;      // target temps set via Grill Config screen
    meat_kind_t   meat_kind;
    probe_state_t probe_state;
    float         grill_temp_c;    // current ambient/grill reading
    float         meat_temp_c;     // current meat reading
    int           grill_target_c;
    int           meat_target_c;
} bbq_grill_t;

void bbq_controller_init(void);

int  bbq_grill_count(void);
// Adds a grill slot with defaults (no probe, not configured). Returns false if
// already at MAX_GRILLS.
bool bbq_add_grill(void);

const bbq_grill_t *bbq_get_grill(int idx);

// Sets the grill's target temps + meat kind from the Grill Config / Meat
// Doneness screens. Real, permanent app state — not a hardware mock.
void bbq_set_targets(int idx, int grill_target_c, int meat_target_c, meat_kind_t kind);

// Applies just the grill target immediately as the Grill Config slider moves,
// without waiting for a meat to be chosen. Leaves the configured flag alone.
void bbq_set_grill_target(int idx, int grill_target_c);

// ---- Temporary mock hooks --------------------------------------------
// No BLE probe hardware/pairing flow exists yet (see project notes on the
// BLE thermometer + wired-thermocouple satellite work). These simulate the
// probe lifecycle with the demo values from the UI wireframes so the four
// gauge states (no probe / connected-unconfigured / configured-no-reading /
// configured-connected) can be verified on real hardware. Replace with real
// BLE reads once that backend exists.
void bbq_mock_connect_probe(int idx);   // "+" tapped — pairs with demo readings
void bbq_mock_toggle_probe(int idx);    // "Probe Status" tapped — connect/disconnect
