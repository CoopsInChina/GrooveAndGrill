#pragma once

// ============================================================
// Music and Meat — application configuration
// ============================================================

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION        "0.1.0"
#endif

// ---- Display ----
#define DISPLAY_WIDTH           480
#define DISPLAY_HEIGHT          480
#define DISPLAY_CIRCLE_R        240     // visible pixel radius
#define DISPLAY_SAFE_R          210     // keep content inside this radius

// ---- NVS ----
#define NVS_NAMESPACE           "meatmusic"
#define NVS_KEY_WIFI_SSID       "wifi_ssid"
#define NVS_KEY_WIFI_PASS       "wifi_pass"
#define NVS_KEY_SPEAKER_IP      "spkr_ip"
#define NVS_KEY_SPEAKER_NAME    "spkr_name"
#define NVS_KEY_API_SERVER      "api_server"  // cached node-sonos-http-api URL
#define SONOS_API_PORT          5005
#define SONOS_API_PROBE_MS      250           // TCP connect timeout per host
#define NVS_KEY_BRIGHTNESS      "brightness"
#define NVS_KEY_AUTODIM_SEC     "autodim"
#define NVS_KEY_FAV_COUNT       "fav_count"
#define NVS_KEY_FAV_BASE        "fav_"      // keys: fav_0 .. fav_19

// ---- WiFi STA ----
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_CONNECT_RETRIES    3

// ---- WiFi AP (captive portal for WiFi setup) ----
#define WIFI_AP_SSID            "MusicMeat-Setup"
#define WIFI_AP_PASS            ""          // open AP
#define WIFI_AP_CHANNEL         6
#define WIFI_AP_MAX_CONN        2
#define WIFI_AP_IP              "192.168.4.1"

// ---- Sonos ----
#define SONOS_DISCOVERY_TIMEOUT_MS  8000
#define SONOS_POLL_INTERVAL_MS      2000
#define SONOS_MAX_SPEAKERS          16

// Spotify service parameters for direct (server-free) favourite playback.
// sid = the Spotify service id on this Sonos household; sn = account serial.
// These are learned automatically from any playing Spotify track's URI and
// cached; the defaults below match the current system so playback works
// before anything has played. serviceType is derived as (sid<<8)+7.
// Only individual tracks need sn — playlists/albums need only the sid.
#define SPOTIFY_DEFAULT_SID         12
#define SPOTIFY_DEFAULT_SN          5

// ---- Screensaver / auto-dim ----
#define DEFAULT_AUTODIM_SEC         30
#define DEFAULT_BRIGHTNESS          80      // percent
#define DIM_BRIGHTNESS              10      // percent when dimmed
#define DEFAULT_DIM_ENABLED         true
#define DEFAULT_SCREENSAVER_ENABLED false
#define DEFAULT_SCREENSAVER_SEC     300     // 5 minutes
#define NVS_KEY_DIM_ENABLED         "dim_en"
#define NVS_KEY_SCREENSAVER_EN      "ss_en"
#define NVS_KEY_SCREENSAVER_SEC     "ss_sec"

// ---- Favourites ----
#define MAX_FAVOURITES          20
#define MAX_DEVICE_FAVOURITES   20
#define NVS_KEY_DEV_FAV_COUNT   "dfav_cnt"  // u8 stored in NVS
// Per-entry keys: "dfav_Xn" (name) and "dfav_Xc" (cmd), X = 0..MAX_DEVICE_FAVOURITES-1

// ---- Temperature sensor (SHT40, I2C shared bus) ----
#define SHT40_I2C_ADDR          0x44

// ---- BBQ probes (future) ----
#define MAX_BBQ_PROBES          4
