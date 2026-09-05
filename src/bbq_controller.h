#pragma once

#include "app_config.h"
#include "bbq_ble.h"     // BBQ_BLE_CHANNELS, BBQ_BLE_MAX_PROBES
#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Sensor-centric BBQ model
// ----------------------------------------------------------------------------
// The primitive is a *sensor* (a wired thermocouple on the box, or a wireless
// probe reached via the box hub). Each sensor is allocated to a grill number
// and a role — grill-ambient or meat — and, for meat, a meat kind + target.
// A "grill" is a derived view: the set of sensors sharing a grill number.
// Allocations persist to NVS, so a cook survives a reboot.
// ============================================================================

#define BBQ_TC_COUNT      BBQ_BLE_CHANNELS       // 4 wired thermocouples
#define BBQ_PROBE_COUNT   BBQ_BLE_MAX_PROBES     // wireless probe slots
#define MAX_BBQ_SENSORS   (BBQ_TC_COUNT + BBQ_PROBE_COUNT)
#define MAX_GRILLS        MAX_BBQ_PROBES         // logical grill numbers 1..MAX_GRILLS

typedef enum {
    SRC_TC = 0,      // wired thermocouple (hw_id = box channel 0..3)
    SRC_PROBE,       // wireless probe     (hw_id = probe id byte)
} sensor_src_t;

typedef enum {
    ROLE_UNASSIGNED = 0,
    ROLE_GRILL,      // ambient / pit temperature
    ROLE_MEAT,       // internal meat temperature
} sensor_role_t;

// Identifies which meat-icon to show and (via bbq_meat_type_idx) which
// doneness table in meat_temps.h to use. Chicken has a single food-safety
// target and isn't in that table.
typedef enum {
    MEAT_KIND_NONE = 0,
    MEAT_KIND_CHICKEN,
    MEAT_KIND_LAMB,
    MEAT_KIND_PORK,
    MEAT_KIND_BEEF,
} meat_kind_t;

typedef struct {
    sensor_src_t  src;
    uint8_t       hw_id;        // TC channel 0..3, or probe id byte
    bool          present;      // reporting within the freshness window
    float         temp_c;       // latest reading (last-known while alarming)
    // True if this sensor was live at some point since boot and has since
    // stopped reporting — a lost-connection alarm, not just "never set up".
    bool          alarm;
    // ---- allocation (persisted) ----
    uint8_t       grill_num;    // 1..MAX_GRILLS, 0 = unassigned
    sensor_role_t role;
    meat_kind_t   meat_kind;    // when role == ROLE_MEAT
    int           target_c;     // grill ambient target, or meat doneness target
} bbq_sensor_t;

void bbq_controller_init(void);

// ---- BLE source mode -------------------------------------------------------
// Where the display gets its temperatures. BOX (default): passive observer of
// the ESP-BLE-BBQ-Box advertisement. PROBE: connect directly to a single
// wireless BBQ probe (no box). Persisted to NVS; the choice is read once at
// boot to bring up the matching BLE stack, so bbq_source_set() reboots.
typedef enum {
    BBQ_SRC_BOX = 0,     // observer of the BBQ Box gateway
    BBQ_SRC_PROBE,       // direct central to one wireless probe
} bbq_source_t;

bbq_source_t bbq_source_get(void);
void         bbq_source_set(bbq_source_t src);   // persists to NVS (caller reboots)

// ---- Direct-probe slot identity (probe mode only) --------------------------
// A wireless probe's BLE address (hw_id) is technically stable, but with two
// probes it's easy to lose track of "which one is which" across a session —
// the first one you ever see stays "slot 1" (etc.) until its allocation is
// removed, rather than the UI position shuffling by current connection order.
// Assigned the first time a probe is seen connected or found already
// allocated; freed by bbq_sensor_unassign(). Persists to NVS.
int  bbq_probe_slot_of(uint8_t hw_id);              // 0-based slot, or -1 if unknown
bool bbq_probe_slot_get(int slot, uint8_t *hw_id_out); // true + hw_id if `slot` is bonded

// True when the active BLE link is up: the box is being heard (BOX mode) or the
// wireless probe is connected (PROBE mode). Drives the on-screen BT indicator.
bool bbq_link_up(void);

// Pause/resume BLE scanning for the active source (dispatches to bbq_ble or
// ble_probe) — gives WiFi the radio to itself, without the caller needing to
// know which BLE stack is active. Used during OTA downloads and while the
// WiFi setup AP is up: continuous BLE scanning shares the radio via
// coexistence and can delay WiFi frames enough to break 802.11 auth/assoc
// timing or DHCP delivery — intermittent connect failures traced to this.
void bbq_radio_pause(bool pause);

// ---- Sensor pool -----------------------------------------------------------
// Count of sensors currently known: every wired channel that is present, plus
// every sensor (wired or wireless) that has a saved allocation.
int  bbq_sensor_count(void);
bool bbq_sensor_at(int i, bbq_sensor_t *out);
bool bbq_sensor_get(sensor_src_t src, uint8_t hw_id, bbq_sensor_t *out);

// Assign / update a sensor's allocation (from the web page or an on-screen
// config flow). Persists to NVS. Passing ROLE_UNASSIGNED clears it.
void bbq_sensor_assign(sensor_src_t src, uint8_t hw_id, uint8_t grill_num,
                       sensor_role_t role, meat_kind_t kind, int target_c);
void bbq_sensor_unassign(sensor_src_t src, uint8_t hw_id);

// Wipe every allocation (clears NVS) — a clean slate. The BBQ screen falls back
// to just the "Add Meat" slot.
void bbq_clear_all(void);

// ---- Derived cook views ----------------------------------------------------
// The gauge screens page through these: one view per allocated MEAT sensor
// (carrying its grill's shared ambient), plus one ambient-only view per grill
// that has a grill sensor but no meat. Grill numbers with nothing allocated
// produce no view.
typedef struct {
    uint8_t       grill_num;
    // grill ambient (shared by every meat on this grill):
    bool          grill_assigned;
    bool          grill_present;
    bool          grill_alarm;    // was live, now not reporting
    float         grill_temp_c;   // last-known while alarming
    int           grill_target_c;
    sensor_src_t  grill_src;      // ambient sensor identity (when grill_assigned)
    uint8_t       grill_hw_id;
    // the meat for this view (has_meat == false → ambient-only view):
    bool          has_meat;
    sensor_src_t  meat_src;
    uint8_t       meat_hw_id;
    meat_kind_t   meat_kind;
    bool          meat_present;
    bool          meat_alarm;     // was live, now not reporting
    float         meat_temp_c;    // last-known while alarming
    int           meat_target_c;
} bbq_view_t;

int  bbq_view_count(void);
bool bbq_view_at(int i, bbq_view_t *out);

// Index of the view containing this sensor (as its meat, or as the ambient of
// a meat-less grill), or -1 if it has no view (e.g. a grill-temp sensor whose
// grill also has a meat — the ambient shows on the meat's view instead).
int  bbq_view_index_for(sensor_src_t src, uint8_t hw_id);

// False only in probe mode once every direct-probe slot is both bonded to a
// physical probe AND allocated to a meat — there is nothing left to add until
// one is removed. Always true in box mode.
bool bbq_can_add_more(void);

// meat_kind → index into MEAT_TYPES (meat_temps.h), or -1 for none/chicken.
int  bbq_meat_type_idx(meat_kind_t k);

// True if grill_num already has a ROLE_GRILL (ambient) sensor assigned; if so
// and src/hw_id are non-NULL, fills that sensor's identity.
bool bbq_grill_has_ambient(uint8_t grill_num, sensor_src_t *src, uint8_t *hw_id);

// ---- On-screen add/edit wizard context -------------------------------
// Carries which sensor an on-screen config flow (Add Meat wizard, or the ⚙
// button on a cook view) is about to assign, so the config/doneness screens
// write the right sensor via bbq_sensor_assign() instead of a fixed mapping.
typedef struct {
    uint8_t       grill_num;
    sensor_src_t  src;
    uint8_t       hw_id;
    sensor_role_t role;       // ROLE_GRILL or ROLE_MEAT
} bbq_setup_t;

void bbq_setup_set(const bbq_setup_t *s);
bool bbq_setup_get(bbq_setup_t *out);

// ============================================================================
// Legacy grill API (compatibility shim)
// ----------------------------------------------------------------------------
// The current gauge / config / doneness screens are still grill-centric. These
// map the old per-grill view onto the sensor model so those screens keep
// working until the one-screen-per-meat UI reframe lands. New code should use
// the sensor / view API above.
// ============================================================================

typedef enum {
    PROBE_NONE = 0,
    PROBE_CONNECTED,
    PROBE_DISCONNECTED,
} probe_state_t;

typedef struct {
    bool          configured;
    meat_kind_t   meat_kind;
    probe_state_t probe_state;
    float         grill_temp_c;
    float         meat_temp_c;
    int           grill_target_c;
    int           meat_target_c;
} bbq_grill_t;

int  bbq_grill_count(void);
bool bbq_add_grill(void);
const bbq_grill_t *bbq_get_grill(int idx);
void bbq_set_targets(int idx, int grill_target_c, int meat_target_c, meat_kind_t kind);
void bbq_set_grill_target(int idx, int grill_target_c);

// Legacy demo hooks — retained so the existing UI's "+"/probe-status controls
// still link. With real BLE data these are no-ops on live sensors.
void bbq_mock_connect_probe(int idx);
void bbq_mock_toggle_probe(int idx);
