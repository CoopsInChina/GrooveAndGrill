#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "app_config.h"

// ============================================================
// Sonos controller — UPnP discovery and playback control
// Ported from SonosESP (Arduino/C++) to ESP-IDF C.
// ============================================================

// ---- State types --------------------------------------------------

typedef enum {
    SONOS_STATE_UNKNOWN,
    SONOS_STATE_PLAYING,
    SONOS_STATE_PAUSED,
    SONOS_STATE_STOPPED,
} sonos_play_state_t;

typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    char album_art_url[512];
    char rel_time[16];         // "H:MM:SS"
    char duration[16];         // "H:MM:SS"
    bool is_radio;
    sonos_play_state_t state;
    uint8_t  volume;           // 0-100
    bool     shuffle;
    bool     valid;            // true once first poll succeeds
} sonos_track_t;

typedef struct {
    char name[64];
    char ip[24];
    int  error_count;
    bool connected;
} sonos_speaker_t;

// ---- Command types (internal, but exposed for queue inspection) ----

typedef enum {
    CMD_PLAY,
    CMD_PAUSE,
    CMD_TOGGLE,
    CMD_NEXT,
    CMD_PREV,
    CMD_SET_VOLUME,
    CMD_PLAY_FAV,
} sonos_cmd_type_t;

typedef struct {
    sonos_cmd_type_t type;
    int              arg;      // volume level or fav index
} sonos_cmd_t;

// ---- Lifecycle ----------------------------------------------------

// Initialise internal state (call once at boot, before WiFi connects)
void sonos_controller_init(void);

// Discover Sonos speakers. Blocks for up to timeout_ms.
// Returns true if at least one speaker found.
bool sonos_controller_discover(uint32_t timeout_ms);

// Start background polling + command tasks (call after discover)
void sonos_controller_start_polling(void);

// Signal from wifi_manager: WiFi just reconnected — probe speaker immediately
void sonos_on_wifi_ready(void);

// ---- Playback control (non-blocking — posts to internal queue) -----

void sonos_play(void);
void sonos_pause(void);
void sonos_toggle_play_pause(void);
void sonos_next(void);
void sonos_prev(void);
void sonos_set_volume(uint8_t vol);
void sonos_play_favourite(uint8_t index);

// ---- node-sonos-http-api server discovery -------------------------

// Check cached server; if unreachable start background /24 subnet scan.
// my_ip: device's own IP string (e.g. "192.168.1.100").
// Returns true immediately if a reachable server was found in NVS cache.
bool sonos_api_server_init(const char *my_ip);

// True if g_api_server is set (either from NVS or background scan).
bool sonos_api_server_ready(void);

// ---- State accessors (thread-safe copies) -------------------------

void        sonos_get_track(sonos_track_t *out);
int         sonos_get_speakers(sonos_speaker_t *out, int max_count);
void        sonos_select_speaker(int index);
const char *sonos_active_speaker_name(void);
bool        sonos_is_connected(void);
// Merged count: Sonos built-in favourites (0..N-1) then device custom (N..total-1)
int         sonos_favourites_count(void);
const char *sonos_favourite_name(int index);
const char *sonos_favourite_art(int index);  // "" if none; absolute or resolved URL

// Device custom favourites management (stored in NVS)
// cmd is the node-sonos-http-api path after /<room>/, e.g. "spotify/now/spotify:album:xxx"
bool        sonos_add_device_favourite(const char *name, const char *cmd);
bool        sonos_remove_device_favourite(int index);
int         sonos_device_fav_count(void);
const char *sonos_device_fav_name(int index);
const char *sonos_device_fav_cmd(int index);
void        sonos_play_device_favourite(int idx);

// Art cache: pre-fetched JPEG bytes stored in SPIFFS, loaded into PSRAM at boot.
// idx is the device-favourite index (0-based within custom favourites only).
bool           sonos_set_device_fav_art(int idx, const uint8_t *jpeg, size_t sz);
const uint8_t *sonos_device_fav_art_data(int idx);  // NULL if no art
size_t         sonos_device_fav_art_size(int idx);
