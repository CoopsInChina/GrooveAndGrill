/**
 * Sonos Controller — ESP-IDF C port of SonosESP
 *
 * Key differences from SonosESP (Arduino/C++):
 *   HTTPClient  → esp_http_client
 *   WiFiUDP     → lwip UDP sockets
 *   String      → char[] static buffers
 *   Preferences → nvs_handle_t
 *   millis()    → esp_timer_get_time() / 1000
 *
 * Favourites are played via node-sonos-http-api (REST).
 * All other control (play/pause/skip/volume/state) uses UPnP SOAP direct to speaker.
 */

#include "sonos_controller.h"
#include "globals.h"
#include "ui_network_guard.h"
#include "app_config.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_spiffs.h"
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "sonos";

// ---- Helpers -------------------------------------------------------

static inline uint32_t ms_now(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ---- State ---------------------------------------------------------

#define SOAP_RESP_BUF_SZ    4096
#define SOAP_BODY_BUF_SZ    3072   // AddURIToQueue body carries encoded metadata
#define HTTP_RESP_BUF_SZ    4096
#define MAX_DEVICES         SONOS_MAX_SPEAKERS

static sonos_speaker_t s_speakers[MAX_DEVICES];
static int             s_speaker_count     = 0;
static int             s_active_idx        = -1;

static sonos_track_t   s_track             = {0};
static SemaphoreHandle_t s_track_mutex     = NULL;

static QueueHandle_t   s_cmd_queue         = NULL;
static TaskHandle_t    s_poll_task         = NULL;
static TaskHandle_t    s_cmd_task          = NULL;

static volatile bool   s_need_reconnect    = false;

// Sonos built-in favourites: fetched from node-sonos-http-api
// EXT_RAM_BSS_ATTR → PSRAM to free internal DRAM for task stacks
static EXT_RAM_BSS_ATTR char s_fav_names[MAX_FAVOURITES][64];
static EXT_RAM_BSS_ATTR char s_fav_art[MAX_FAVOURITES][256];
static int  s_fav_count = 0;

// Device-stored custom favourites (NVS). cmd is the node-sonos-http-api command
// (e.g. "spotify/now/spotify:playlist:<id>"); played directly via SOAP when
// possible, falling back to the API server otherwise.
static EXT_RAM_BSS_ATTR char s_dev_fav_names[MAX_DEVICE_FAVOURITES][64];
static EXT_RAM_BSS_ATTR char s_dev_fav_cmds[MAX_DEVICE_FAVOURITES][256];
static int  s_dev_fav_count = 0;

// Spotify service params for direct favourite playback — learned from a playing
// Spotify track URI (see update_track_info), defaulting to this system's values.
static int  s_spotify_sid = SPOTIFY_DEFAULT_SID;
static int  s_spotify_sn  = SPOTIFY_DEFAULT_SN;

// Art cache: JPEG bytes per custom favourite, loaded from SPIFFS into PSRAM at boot.
// 20 × 25 KB = 500 KB PSRAM — fine on 8 MB PSRAM board.
#define ART_MAX_BYTES  (64 * 1024)
static EXT_RAM_BSS_ATTR uint8_t s_dev_fav_art_data[MAX_DEVICE_FAVOURITES][ART_MAX_BYTES];
static size_t                   s_dev_fav_art_sz[MAX_DEVICE_FAVOURITES];

// HTTP response buffer for fetch_favourites — in PSRAM, not DRAM
static EXT_RAM_BSS_ATTR char s_fav_fetch_resp[HTTP_RESP_BUF_SZ];

// Browse FV:2 response buffer — DIDL-Lite can be 15-30 KB for 20 favourites
#define BROWSE_RESP_BUF_SZ  32768
static EXT_RAM_BSS_ATTR char s_browse_resp[BROWSE_RESP_BUF_SZ];

// Set by save_api_server() so poll_task triggers a favourites fetch
static volatile bool s_fetch_favs_requested = false;
// True while a fetch_favs_task is alive — prevents concurrent fetches
static volatile bool s_fetch_task_running   = false;

// ---- XML extraction ------------------------------------------------

static bool extract_xml(const char *xml, const char *tag, char *out, size_t out_sz)
{
    char open[80], close[84];
    snprintf(open,  sizeof(open),  "<%s>",  tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    const char *s = strstr(xml, open);
    if (!s) return false;
    s += strlen(open);

    const char *e = strstr(s, close);
    if (!e) return false;

    size_t len = (size_t)(e - s);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, s, len);
    out[len] = '\0';
    return true;
}

// XML-entity-encode a value for inclusion in a SOAP argument body. Needed for
// URIs (contain '&') and DIDL metadata (contains '<','>','&') passed to
// AddURIToQueue / SetAVTransportURI.
static void xml_encode(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 6 < dst_sz; p++) {
        switch (*p) {
            case '&':  memcpy(dst + o, "&amp;",  5); o += 5; break;
            case '<':  memcpy(dst + o, "&lt;",   4); o += 4; break;
            case '>':  memcpy(dst + o, "&gt;",   4); o += 4; break;
            case '"':  memcpy(dst + o, "&quot;", 6); o += 6; break;
            case '\'': memcpy(dst + o, "&apos;", 6); o += 6; break;
            default:   dst[o++] = *p; break;
        }
    }
    dst[o] = '\0';
}

static void decode_html_entities(char *s)
{
    static const struct { const char *from; const char *to; } ent[] = {
        { "&amp;",  "&"  }, { "&lt;",   "<"  }, { "&gt;",   ">"  },
        { "&quot;", "\"" }, { "&apos;", "'"  }, { "&#39;",  "'"  },
        { "&#233;", "e"  }, { "&#232;", "e"  }, { "&#224;", "a"  },
    };
    for (int i = 0; i < (int)(sizeof(ent)/sizeof(ent[0])); i++) {
        char *p;
        size_t flen = strlen(ent[i].from), tlen = strlen(ent[i].to);
        while ((p = strstr(s, ent[i].from)) != NULL) {
            memmove(p + tlen, p + flen, strlen(p + flen) + 1);
            memcpy(p, ent[i].to, tlen);
        }
    }
}

static void parse_stream_content(const char *content, char *artist, size_t asiz,
                                  char *title, size_t tsiz)
{
    if (!content || !content[0]) return;

    if (strstr(content, "TYPE=") && strchr(content, '|')) {
        const char *tp = strstr(content, "TITLE ");
        if (tp) {
            tp += 6;
            const char *te = strchr(tp, '|');
            size_t len = te ? (size_t)(te - tp) : strlen(tp);
            if (len >= tsiz) len = tsiz - 1;
            memcpy(title, tp, len); title[len] = '\0';
        }
        const char *ap = strstr(content, "ARTIST ");
        if (ap) {
            ap += 7;
            const char *ae = strchr(ap, '|');
            size_t len = ae ? (size_t)(ae - ap) : strlen(ap);
            if (len >= asiz) len = asiz - 1;
            memcpy(artist, ap, len); artist[len] = '\0';
        }
        return;
    }

    const char *sep = strstr(content, " - ");
    if (sep) {
        size_t alen = (size_t)(sep - content);
        if (alen >= asiz) alen = asiz - 1;
        memcpy(artist, content, alen); artist[alen] = '\0';
        strncpy(title, sep + 3, tsiz - 1);
        return;
    }

    strncpy(title, content, tsiz - 1);
}

static bool is_radio_uri(const char *uri)
{
    return strncmp(uri, "x-sonosapi-stream:", 18) == 0 ||
           strncmp(uri, "x-rincon-mp3radio:", 18) == 0 ||
           strncmp(uri, "x-sonosapi-radio:",  17) == 0 ||
           strncmp(uri, "x-sonosapi-hls:",    15) == 0 ||
           strncmp(uri, "aac://",              6)  == 0 ||
           strncmp(uri, "hls-radio:",         10) == 0;
}

// ---- HTTP event handler for response body --------------------------

typedef struct {
    char  *buf;
    int    len;
    int    max;
} http_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (!ctx || !ctx->buf) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (ctx->len + copy > ctx->max - 1) copy = ctx->max - 1 - ctx->len;
        if (copy > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, copy);
            ctx->len += copy;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

// ---- SOAP request --------------------------------------------------

static int send_soap(const char *service, const char *action,
                     const char *args, char *resp_buf, size_t resp_sz)
{
    if (s_active_idx < 0 || s_active_idx >= s_speaker_count) return -1;
    const char *ip = s_speakers[s_active_idx].ip;

    const char *endpoint;
    static char custom_ep[128];
    if (strstr(service, "AVTransport"))
        endpoint = "/MediaRenderer/AVTransport/Control";
    else if (strstr(service, "RenderingControl"))
        endpoint = "/MediaRenderer/RenderingControl/Control";
    else if (strstr(service, "ContentDirectory"))
        endpoint = "/MediaServer/ContentDirectory/Control";
    else {
        snprintf(custom_ep, sizeof(custom_ep), "/MediaRenderer/%s/Control", service);
        endpoint = custom_ep;
    }

    static char url[256];
    snprintf(url, sizeof(url), "http://%s:1400%s", ip, endpoint);

    static char body[SOAP_BODY_BUF_SZ];
    snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%s xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">%s</u:%s>"
        "</s:Body></s:Envelope>",
        action, service, args, action);

    static char soap_action[280];
    snprintf(soap_action, sizeof(soap_action),
             "\"urn:schemas-upnp-org:service:%s:1#%s\"", service, action);

    net_pre_wait("SOAP", NET_WAIT_GENERAL);
    if (!xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(NETWORK_MUTEX_TIMEOUT_MS))) {
        ESP_LOGW(TAG, "SOAP: mutex timeout for %s.%s", service, action);
        return -1;
    }

    static char resp_tmp[SOAP_RESP_BUF_SZ];
    http_ctx_t ctx = { resp_tmp, 0, (int)sizeof(resp_tmp) };
    resp_tmp[0] = '\0';

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 3000,
        .event_handler = http_event_handler,
        .user_data     = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");
    esp_http_client_set_header(client, "SOAPAction",   soap_action);
    esp_http_client_set_header(client, "Connection",   "close");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err  = esp_http_client_perform(client);
    int       code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    g_last_network_end_ms = ms_now();
    xSemaphoreGive(g_network_mutex);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SOAP HTTP err %d for %s.%s", err, service, action);
        s_speakers[s_active_idx].error_count++;
        if (s_speakers[s_active_idx].error_count > 5)
            s_speakers[s_active_idx].connected = false;
        return -1;
    }

    if (code == 200) {
        s_speakers[s_active_idx].error_count = 0;
        s_speakers[s_active_idx].connected   = true;
        if (resp_buf && resp_sz > 0) {
            strncpy(resp_buf, resp_tmp, resp_sz - 1);
            resp_buf[resp_sz - 1] = '\0';
        }
    } else if (code == 500) {
        ESP_LOGD(TAG, "SOAP 500 (transient) %s.%s", service, action);
    } else {
        ESP_LOGW(TAG, "SOAP %d for %s.%s", code, service, action);
        s_speakers[s_active_idx].error_count++;
    }

    return code;
}

// ---- Room name fetch -----------------------------------------------

// Fetches roomName and the RINCON uuid from the player's device description.
// uuid_buf may be NULL if not needed.
static void fetch_room_name(const char *ip, char *name_buf, size_t name_sz,
                            char *uuid_buf, size_t uuid_sz)
{
    static char url[128];
    snprintf(url, sizeof(url), "http://%s:1400/xml/device_description.xml", ip);

    static char resp[2048];
    http_ctx_t ctx = { resp, 0, (int)sizeof(resp) };
    resp[0] = '\0';

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_GET,
        .timeout_ms    = 3000,
        .event_handler = http_event_handler,
        .user_data     = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int code      = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && code == 200) {
        extract_xml(resp, "roomName", name_buf, name_sz);
        if (uuid_buf && uuid_sz) {
            // <UDN>uuid:RINCON_XXXXXXXXXXXX01400</UDN> — strip the "uuid:" prefix.
            char udn[64] = {0};
            if (extract_xml(resp, "UDN", udn, sizeof(udn))) {
                const char *r = strstr(udn, "RINCON");
                strncpy(uuid_buf, r ? r : udn, uuid_sz - 1);
                uuid_buf[uuid_sz - 1] = '\0';
            }
        }
    } else {
        strncpy(name_buf, ip, name_sz - 1);
    }
}

// ---- Discovery (SSDP via lwip UDP) ---------------------------------

bool sonos_controller_discover(uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "Discovery starting (%lums)...", (unsigned long)timeout_ms);
    s_speaker_count = 0;
    s_active_idx    = -1;

    if (!xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(20000))) {
        ESP_LOGE(TAG, "Discovery: could not acquire network mutex");
        return false;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Discovery socket() failed: %d", errno);
        xSemaphoreGive(g_network_mutex);
        return false;
    }

    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = 0,
    };
    bind(sock, (struct sockaddr *)&local, sizeof(local));

    const char *msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";

    struct sockaddr_in mcast = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = inet_addr("239.255.255.250"),
        .sin_port        = htons(1900),
    };
    struct sockaddr_in bcast_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = inet_addr("255.255.255.255"),
        .sin_port        = htons(1900),
    };

    for (int burst = 0; burst < 3; burst++) {
        sendto(sock, msearch, strlen(msearch), 0,
               (struct sockaddr *)&mcast,      sizeof(mcast));
        sendto(sock, msearch, strlen(msearch), 0,
               (struct sockaddr *)&bcast_addr, sizeof(bcast_addr));
        ESP_LOGI(TAG, "SSDP burst %d/3", burst + 1);
        if (burst < 2) vTaskDelay(pdMS_TO_TICKS(500));
    }

    uint32_t start = ms_now();
    char buf[1025];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    while (ms_now() - start < timeout_ms) {
        int len = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                           (struct sockaddr *)&from, &from_len);
        if (len <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        buf[len] = '\0';

        // Case-insensitive search for Sonos signature
        char lower[1025];
        for (int i = 0; i <= len; i++)
            lower[i] = (char)tolower((unsigned char)buf[i]);
        if (!strstr(lower, "sonos") && !strstr(lower, "zoneplayer")) continue;

        char ip_str[24];
        snprintf(ip_str, sizeof(ip_str), "%s", inet_ntoa(from.sin_addr));

        bool exists = false;
        for (int i = 0; i < s_speaker_count; i++)
            if (strcmp(s_speakers[i].ip, ip_str) == 0) { exists = true; break; }
        if (exists || s_speaker_count >= MAX_DEVICES) continue;

        strncpy(s_speakers[s_speaker_count].ip, ip_str,
                sizeof(s_speakers[s_speaker_count].ip) - 1);
        s_speakers[s_speaker_count].connected   = false;
        s_speakers[s_speaker_count].error_count = 0;
        ESP_LOGI(TAG, "Found: %s", ip_str);
        s_speaker_count++;
    }

    close(sock);
    g_last_network_end_ms = ms_now();
    xSemaphoreGive(g_network_mutex);

    ESP_LOGI(TAG, "Discovery: %d device(s) found", s_speaker_count);
    if (s_speaker_count == 0) {
        // SSDP found nothing — fall back to last-known IP from NVS
        char saved_ip[24] = {0};
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(saved_ip);
            nvs_get_str(nvs, NVS_KEY_SPEAKER_IP, saved_ip, &len);
            nvs_close(nvs);
        }
        if (!saved_ip[0]) {
            ESP_LOGI(TAG, "Discovery: no speakers found, no NVS fallback");
            return false;
        }
        ESP_LOGI(TAG, "Discovery: SSDP found 0, probing NVS IP %s", saved_ip);
        memset(&s_speakers[0], 0, sizeof(s_speakers[0]));
        strncpy(s_speakers[0].ip, saved_ip, sizeof(s_speakers[0].ip) - 1);
        fetch_room_name(saved_ip, s_speakers[0].name, sizeof(s_speakers[0].name),
                        s_speakers[0].uuid, sizeof(s_speakers[0].uuid));
        s_speakers[0].connected   = true;
        s_speakers[0].error_count = 0;
        s_speaker_count = 1;
        s_active_idx    = 0;
        ESP_LOGI(TAG, "NVS fallback: active speaker %s (%s)",
                 s_speakers[0].name, saved_ip);
        return true;
    }

    // Fetch room names + uuids
    for (int i = 0; i < s_speaker_count; i++) {
        fetch_room_name(s_speakers[i].ip,
                        s_speakers[i].name, sizeof(s_speakers[i].name),
                        s_speakers[i].uuid, sizeof(s_speakers[i].uuid));
        ESP_LOGI(TAG, "  [%d] %s (%s) %s", i, s_speakers[i].name,
                 s_speakers[i].ip, s_speakers[i].uuid);
    }

    // Restore cached speaker from NVS
    char saved_ip[24] = {0};
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(saved_ip);
        nvs_get_str(nvs, NVS_KEY_SPEAKER_IP, saved_ip, &len);
        nvs_close(nvs);
    }

    s_active_idx = 0;
    if (saved_ip[0]) {
        for (int i = 0; i < s_speaker_count; i++) {
            if (strcmp(s_speakers[i].ip, saved_ip) == 0) {
                s_active_idx = i;
                ESP_LOGI(TAG, "Restored cached speaker: %s", s_speakers[i].name);
                break;
            }
        }
    }

    s_speakers[s_active_idx].connected = true;
    return true;
}

// ---- State polling -------------------------------------------------

static void update_track_info(void)
{
    static char resp[SOAP_RESP_BUF_SZ];
    if (send_soap("AVTransport", "GetPositionInfo",
                  "<InstanceID>0</InstanceID>", resp, sizeof(resp)) != 200) return;

    char uri[256]       = {0};
    char meta_raw[2048] = {0};
    char rel_time[16]   = {0};
    char duration[16]   = {0};
    extract_xml(resp, "TrackURI",       uri,       sizeof(uri));

    // Learn Spotify service params from a playing Spotify track URI, e.g.
    // x-sonos-spotify:spotify%3atrack%3a...?sid=12&flags=8232&sn=5
    if (strstr(uri, "x-sonos-spotify")) {
        const char *p;
        if ((p = strstr(uri, "sid=")) != NULL) { int v = atoi(p + 4); if (v > 0) s_spotify_sid = v; }
        if ((p = strstr(uri, "sn="))  != NULL) { int v = atoi(p + 3); if (v > 0) s_spotify_sn  = v; }
    }

    extract_xml(resp, "TrackMetaData",  meta_raw,  sizeof(meta_raw));
    extract_xml(resp, "RelTime",        rel_time,  sizeof(rel_time));
    extract_xml(resp, "TrackDuration",  duration,  sizeof(duration));
    decode_html_entities(meta_raw);

    char new_title[128]  = {0};
    char new_artist[128] = {0};
    char new_album[128]  = {0};
    char new_art[512]    = {0};
    bool radio = is_radio_uri(uri);

    if (radio) {
        char stream_content[256] = {0};
        extract_xml(meta_raw, "r:streamContent", stream_content, sizeof(stream_content));
        if (stream_content[0])
            parse_stream_content(stream_content, new_artist, sizeof(new_artist),
                                 new_title, sizeof(new_title));
        else
            extract_xml(meta_raw, "dc:title", new_title, sizeof(new_title));
        char raw_art[400] = {0};
        extract_xml(meta_raw, "upnp:albumArtURI", raw_art, sizeof(raw_art));
        if (raw_art[0] == '/') {
            snprintf(new_art, sizeof(new_art), "http://%s:1400%s",
                     s_speakers[s_active_idx].ip, raw_art);
        } else if (raw_art[0] != '\0') {
            strncpy(new_art, raw_art, sizeof(new_art) - 1);
        }
    } else {
        char raw_art[400] = {0};
        extract_xml(meta_raw, "dc:title",        new_title,  sizeof(new_title));
        extract_xml(meta_raw, "dc:creator",       new_artist, sizeof(new_artist));
        extract_xml(meta_raw, "upnp:album",       new_album,  sizeof(new_album));
        extract_xml(meta_raw, "upnp:albumArtURI", raw_art,    sizeof(raw_art));
        if (raw_art[0] == '/')
            snprintf(new_art, sizeof(new_art), "http://%s:1400%s",
                     s_speakers[s_active_idx].ip, raw_art);
        else
            strncpy(new_art, raw_art, sizeof(new_art) - 1);
    }

    if (xSemaphoreTake(s_track_mutex, pdMS_TO_TICKS(50))) {
        strncpy(s_track.title,         new_title,  sizeof(s_track.title)  - 1);
        strncpy(s_track.artist,        new_artist, sizeof(s_track.artist) - 1);
        strncpy(s_track.album,         new_album,  sizeof(s_track.album)  - 1);
        strncpy(s_track.album_art_url, new_art,    sizeof(s_track.album_art_url) - 1);
        strncpy(s_track.rel_time,      rel_time,   sizeof(s_track.rel_time)  - 1);
        strncpy(s_track.duration,      duration,   sizeof(s_track.duration)  - 1);
        s_track.is_radio = radio;
        s_track.valid    = true;
        xSemaphoreGive(s_track_mutex);
    }
}

static void update_playback_state(void)
{
    static char resp[SOAP_RESP_BUF_SZ];
    if (send_soap("AVTransport", "GetTransportInfo",
                  "<InstanceID>0</InstanceID>", resp, sizeof(resp)) != 200) return;

    sonos_play_state_t state;
    if (strstr(resp, "PLAYING"))     state = SONOS_STATE_PLAYING;
    else if (strstr(resp, "PAUSED")) state = SONOS_STATE_PAUSED;
    else                             state = SONOS_STATE_STOPPED;

    if (xSemaphoreTake(s_track_mutex, pdMS_TO_TICKS(50))) {
        s_track.state = state;
        xSemaphoreGive(s_track_mutex);
    }
}

static void update_volume(void)
{
    static char resp[SOAP_RESP_BUF_SZ];
    if (send_soap("RenderingControl", "GetVolume",
                  "<InstanceID>0</InstanceID><Channel>Master</Channel>",
                  resp, sizeof(resp)) != 200) return;

    char vol_str[8] = {0};
    if (extract_xml(resp, "CurrentVolume", vol_str, sizeof(vol_str))) {
        if (xSemaphoreTake(s_track_mutex, pdMS_TO_TICKS(50))) {
            s_track.volume = (uint8_t)atoi(vol_str);
            xSemaphoreGive(s_track_mutex);
        }
    }
}

// ---- Favourites via node-sonos-http-api ----------------------------

// URL-encode a room name (spaces → %20, and other unsafe chars).
static void url_encode_room(const char *in, char *out, size_t out_sz)
{
    size_t oi = 0;
    for (const char *p = in; *p && oi + 4 < out_sz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ') {
            out[oi++] = '%'; out[oi++] = '2'; out[oi++] = '0';
        } else if (c > 127 || strchr("?#[]@!$&'()*+,;=", (char)c)) {
            out[oi++] = '%';
            out[oi++] = "0123456789ABCDEF"[c >> 4];
            out[oi++] = "0123456789ABCDEF"[c & 0xF];
        } else {
            out[oi++] = (char)c;
        }
    }
    out[oi] = '\0';
}

// ---- Device custom favourites (NVS) --------------------------------

// ---- Art SPIFFS helpers --------------------------------------------

static void spiffs_art_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/art",
        .partition_label        = "art_cache",
        .max_files              = MAX_DEVICE_FAVOURITES + 2,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    else
        ESP_LOGI(TAG, "Art SPIFFS mounted at /art");
}

static void spiffs_art_path(int idx, char *buf, size_t sz)
{
    snprintf(buf, sz, "/art/fav_%d.jpg", idx);
}

static void load_art_from_spiffs(void)
{
    char path[32];
    for (int i = 0; i < s_dev_fav_count; i++) {
        spiffs_art_path(i, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) { s_dev_fav_art_sz[i] = 0; continue; }
        size_t n = fread(s_dev_fav_art_data[i], 1, ART_MAX_BYTES, f);
        fclose(f);
        s_dev_fav_art_sz[i] = n;
        ESP_LOGI(TAG, "Art[%d]: %zu bytes from SPIFFS", i, n);
    }
}

// ---- Device custom favourites (NVS) --------------------------------

static void save_device_favourites(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, NVS_KEY_DEV_FAV_COUNT, (uint8_t)s_dev_fav_count);
    char key[20];
    for (int i = 0; i < s_dev_fav_count; i++) {
        snprintf(key, sizeof(key), "dfav_%dn", i);
        nvs_set_str(nvs, key, s_dev_fav_names[i]);
        snprintf(key, sizeof(key), "dfav_%dc", i);
        nvs_set_str(nvs, key, s_dev_fav_cmds[i]);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void load_device_favourites(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t cnt = 0;
    nvs_get_u8(nvs, NVS_KEY_DEV_FAV_COUNT, &cnt);
    if (cnt > MAX_DEVICE_FAVOURITES) cnt = MAX_DEVICE_FAVOURITES;
    char key[20];
    s_dev_fav_count = 0;
    for (int i = 0; i < (int)cnt; i++) {
        size_t n_len = sizeof(s_dev_fav_names[s_dev_fav_count]);
        size_t c_len = sizeof(s_dev_fav_cmds[s_dev_fav_count]);
        snprintf(key, sizeof(key), "dfav_%dn", i);
        if (nvs_get_str(nvs, key, s_dev_fav_names[s_dev_fav_count], &n_len) != ESP_OK) continue;
        snprintf(key, sizeof(key), "dfav_%dc", i);
        if (nvs_get_str(nvs, key, s_dev_fav_cmds[s_dev_fav_count], &c_len) != ESP_OK) continue;
        s_dev_fav_count++;
    }
    nvs_close(nvs);
    ESP_LOGI(TAG, "Loaded %d device favourites", s_dev_fav_count);
    load_art_from_spiffs();
}

// ---- Sonos built-in favourites (API server) ------------------------

static bool fetch_favourites(void)
{
    if (!g_api_server[0]) {
        ESP_LOGW(TAG, "fetch_fav: no api server");
        return true;  // not a transient failure — no point retrying
    }
    static char url[128];
    snprintf(url, sizeof(url), "http://%s/favorites", g_api_server);
    ESP_LOGI(TAG, "fetch_fav: GET %s", url);

    char *resp = s_fav_fetch_resp;
    http_ctx_t ctx = { resp, 0, HTTP_RESP_BUF_SZ };
    resp[0] = '\0';

    if (!xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(6000))) {
        ESP_LOGW(TAG, "fetch_fav: mutex timeout");
        return false;
    }
    net_pre_wait("FAV", NET_WAIT_GENERAL);

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_GET,
        .timeout_ms    = 5000,
        .event_handler = http_event_handler,
        .user_data     = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "URL parse failed: %s", url);
        g_last_network_end_ms = ms_now();
        xSemaphoreGive(g_network_mutex);
        return true;
    }
    esp_err_t err  = esp_http_client_perform(client);
    int       code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    g_last_network_end_ms = ms_now();
    xSemaphoreGive(g_network_mutex);

    if (err != ESP_OK || code != 200) {
        ESP_LOGW(TAG, "fetch_fav: failed err=%d code=%d", (int)err, code);
        return true;
    }
    ESP_LOGI(TAG, "fetch_fav: resp(%d bytes): %.120s", ctx.len, resp);

    // Parse response — two formats supported:
    // Object array: [{"title":"BBC Radio 2","uri":"..."},...] → extract "title" field
    // Simple array: ["BBC Radio 2",...]                       → extract each string
    s_fav_count = 0;
    const char *p = resp;
    if (strstr(resp, "\"title\"")) {
        // Object format: each element is {"title":"...","albumArtUri":"...",...}
        while (s_fav_count < MAX_FAVOURITES) {
            const char *t = strstr(p, "\"title\"");
            if (!t) break;
            t += 7;
            while (*t == ' ' || *t == '\t') t++;
            if (*t != ':') { p = t; continue; }
            t++;
            while (*t == ' ' || *t == '\t') t++;
            if (*t != '"') { p = t; continue; }
            t++;
            const char *end = strchr(t, '"');
            if (!end) break;
            size_t len = (size_t)(end - t);
            if (len > 0 && len < sizeof(s_fav_names[0])) {
                memcpy(s_fav_names[s_fav_count], t, len);
                s_fav_names[s_fav_count][len] = '\0';
                if (strcasecmp(s_fav_names[s_fav_count], "Favorites") != 0) {
                    // Extract albumArtUri from the same JSON object.
                    // Search from the '{' before this title up to the next '{'.
                    s_fav_art[s_fav_count][0] = '\0';
                    const char *obj = t;
                    while (obj > resp && *obj != '{') obj--;
                    const char *next_obj = strchr(end, '{');
                    const char *ak = strstr(obj, "\"albumArtUri\"");
                    if (ak && (!next_obj || ak < next_obj)) {
                        ak += 13;
                        while (*ak == ' ' || *ak == ':' || *ak == '\t') ak++;
                        if (*ak == '"') {
                            ak++;
                            const char *ae = strchr(ak, '"');
                            if (ae) {
                                size_t al = (size_t)(ae - ak);
                                if (al > 0 && al < sizeof(s_fav_art[0])) {
                                    memcpy(s_fav_art[s_fav_count], ak, al);
                                    s_fav_art[s_fav_count][al] = '\0';
                                }
                            }
                        }
                    }
                    s_fav_count++;
                }
            }
            p = end + 1;
        }
    } else {
        // Simple string array: ["name1","name2",...] — no art available
        while (s_fav_count < MAX_FAVOURITES) {
            const char *q = strchr(p, '"');
            if (!q) break;
            q++;
            const char *end = strchr(q, '"');
            if (!end) break;
            size_t len = (size_t)(end - q);
            if (len > 0 && len < sizeof(s_fav_names[0])) {
                memcpy(s_fav_names[s_fav_count], q, len);
                s_fav_names[s_fav_count][len] = '\0';
                if (strcasecmp(s_fav_names[s_fav_count], "Favorites") != 0) {
                    s_fav_art[s_fav_count][0] = '\0';
                    s_fav_count++;
                }
            }
            p = end + 1;
        }
    }
    ESP_LOGI(TAG, "Loaded %d favourites", s_fav_count);
    return true;
}

// Builds the Sonos URI + DIDL metadata for a Spotify favourite command such as
// "spotify/now/spotify:playlist:<id>". Mirrors node-sonos-http-api's spotify
// action. Returns false for non-Spotify commands (caller falls back to server).
//   - playlist/album: x-rincon-cpcontainer container URI (needs only serviceType)
//   - track:          x-sonos-spotify URI (needs sid + sn)
static bool build_spotify_uri_meta(const char *cmd, char *uri, size_t uri_sz,
                                    char *meta, size_t meta_sz)
{
    const char *sp = strstr(cmd, "spotify:");
    if (!sp) return false;   // not a Spotify command (e.g. applemusic)

    char type[16] = {0}, id[128] = {0};
    if (sscanf(sp, "spotify:%15[^:]:%127s", type, id) != 2) return false;

    // Strip a Spotify share query (?si=...) and any trailing junk (e.g. an
    // encoded newline) that may have been captured when the favourite was
    // added via the web page — the bare id is all Sonos needs.
    char *q = strpbrk(id, "?&");
    if (q) *q = '\0';

    int stype = (s_spotify_sid << 8) + 7;

    // "spotify:TYPE:ID" url-encoded the way Sonos expects (colons -> %3a).
    char enc[192];
    snprintf(enc, sizeof(enc), "spotify%%3a%s%%3a%s", type, id);

    if (strcmp(type, "track") == 0) {
        snprintf(uri, uri_sz, "x-sonos-spotify:%s?sid=%d&flags=8224&sn=%d",
                 enc, s_spotify_sid, s_spotify_sn);
    } else if (strcmp(type, "album") == 0) {
        snprintf(uri, uri_sz, "x-rincon-cpcontainer:0004206c%s", enc);
    } else if (strcmp(type, "playlist") == 0) {
        snprintf(uri, uri_sz, "x-rincon-cpcontainer:0006206c%s", enc);
    } else {
        return false;
    }

    snprintf(meta, meta_sz,
        "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
        "xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" "
        "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
        "<item id=\"00030020%s\" restricted=\"true\">"
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
        "<desc id=\"cdudn\" nameSpace=\"urn:schemas-rinconnetworks-com:metadata-1-0/\">"
        "SA_RINCON%d_X_#Svc%d-0-Token</desc></item></DIDL-Lite>",
        enc, stype, stype);
    return true;
}

// Plays a custom (device) favourite directly via SOAP, no external server.
// Sequence mirrors replaceWithFavorite's non-radio path: clear queue, enqueue
// the favourite, point the transport at the queue, play. Returns false if it
// couldn't be handled directly (caller falls back to the API server).
static bool play_custom_favourite_direct(int di)
{
    if (di < 0 || di >= s_dev_fav_count) return false;
    const char *uuid = s_speakers[s_active_idx].uuid;
    if (!uuid[0]) return false;   // no queue URI possible without the RINCON id

    static char uri[256], meta[768];
    if (!build_spotify_uri_meta(s_dev_fav_cmds[di], uri, sizeof(uri), meta, sizeof(meta)))
        return false;

    static char uri_enc[512], meta_enc[1536], args[3072], resp[512];

    send_soap("AVTransport", "RemoveAllTracksFromQueue",
              "<InstanceID>0</InstanceID>", resp, sizeof(resp));

    xml_encode(uri,  uri_enc,  sizeof(uri_enc));
    xml_encode(meta, meta_enc, sizeof(meta_enc));
    snprintf(args, sizeof(args),
        "<InstanceID>0</InstanceID><EnqueuedURI>%s</EnqueuedURI>"
        "<EnqueuedURIMetaData>%s</EnqueuedURIMetaData>"
        "<DesiredFirstTrackNumberEnqueued>0</DesiredFirstTrackNumberEnqueued>"
        "<EnqueueAsNext>0</EnqueueAsNext>",
        uri_enc, meta_enc);
    if (send_soap("AVTransport", "AddURIToQueue", args, resp, sizeof(resp)) != 200)
        return false;

    // Diagnostic: how many tracks actually landed in the queue.
    char added[8] = {0}, qlen[8] = {0};
    extract_xml(resp, "NumTracksAdded", added, sizeof(added));
    extract_xml(resp, "NewQueueLength", qlen,  sizeof(qlen));
    ESP_LOGI(TAG, "AddURIToQueue: added=%s newLen=%s", added, qlen);

    snprintf(args, sizeof(args),
        "<InstanceID>0</InstanceID>"
        "<CurrentURI>x-rincon-queue:%s#0</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>",
        uuid);
    send_soap("AVTransport", "SetAVTransportURI", args, resp, sizeof(resp));

    // Position the queue at the first enqueued track — a freshly-swapped queue
    // otherwise sits before track 1 and Play does nothing.
    send_soap("AVTransport", "Seek",
              "<InstanceID>0</InstanceID><Unit>TRACK_NR</Unit><Target>1</Target>",
              resp, sizeof(resp));

    int play_code = send_soap("AVTransport", "Play",
                              "<InstanceID>0</InstanceID><Speed>1</Speed>",
                              resp, sizeof(resp));

    ESP_LOGI(TAG, "Play custom fav[%d] direct: %s (play=%d) %s",
             di, s_dev_fav_names[di], play_code, uri);
    return true;
}

static void do_play_favourite(int index)
{
    int total = s_fav_count + s_dev_fav_count;
    if (s_active_idx < 0 || index < 0 || index >= total) return;

    // Custom favourites: try direct SOAP playback first (no external server).
    if (index >= s_fav_count) {
        if (play_custom_favourite_direct(index - s_fav_count)) return;
        // else fall through to the API server path (e.g. Apple Music, or no uuid)
    }

    if (!g_api_server[0]) {
        ESP_LOGW(TAG, "Play favourite[%d]: no direct path and no API server", index);
        return;
    }

    char room_enc[128];
    url_encode_room(s_speakers[s_active_idx].name, room_enc, sizeof(room_enc));

    static char url[512];
    if (index < s_fav_count) {
        char fav_enc[256];
        url_encode_room(s_fav_names[index], fav_enc, sizeof(fav_enc));
        snprintf(url, sizeof(url), "http://%s/%s/favourite/%s",
                 g_api_server, room_enc, fav_enc);
    } else {
        int di = index - s_fav_count;
        snprintf(url, sizeof(url), "http://%s/%s/%s",
                 g_api_server, room_enc, s_dev_fav_cmds[di]);
    }

    if (!xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(3000))) return;
    net_pre_wait("PLAY_FAV", NET_WAIT_GENERAL);

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "URL parse failed: %s", url);
        g_last_network_end_ms = ms_now();
        xSemaphoreGive(g_network_mutex);
        return;
    }
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    g_last_network_end_ms = ms_now();
    xSemaphoreGive(g_network_mutex);

    ESP_LOGI(TAG, "Play favourite[%d]: %s", index, sonos_favourite_name(index));
}

// ---- Direct SOAP commands ------------------------------------------

static void do_soap_cmd(sonos_cmd_type_t type, int arg)
{
    static char resp[512];
    static char vol_args[128];

    switch (type) {
        case CMD_PLAY:
            send_soap("AVTransport", "Play",
                      "<InstanceID>0</InstanceID><Speed>1</Speed>", resp, sizeof(resp));
            break;
        case CMD_PAUSE:
            send_soap("AVTransport", "Pause",
                      "<InstanceID>0</InstanceID>", resp, sizeof(resp));
            break;
        case CMD_TOGGLE: {
            sonos_play_state_t st = SONOS_STATE_UNKNOWN;
            if (xSemaphoreTake(s_track_mutex, pdMS_TO_TICKS(20))) {
                st = s_track.state; xSemaphoreGive(s_track_mutex);
            }
            if (st == SONOS_STATE_PLAYING)
                send_soap("AVTransport", "Pause",
                          "<InstanceID>0</InstanceID>", resp, sizeof(resp));
            else
                send_soap("AVTransport", "Play",
                          "<InstanceID>0</InstanceID><Speed>1</Speed>", resp, sizeof(resp));
            break;
        }
        case CMD_NEXT:
            send_soap("AVTransport", "Next",
                      "<InstanceID>0</InstanceID>", resp, sizeof(resp));
            break;
        case CMD_PREV:
            send_soap("AVTransport", "Previous",
                      "<InstanceID>0</InstanceID>", resp, sizeof(resp));
            break;
        case CMD_SET_VOLUME:
            snprintf(vol_args, sizeof(vol_args),
                     "<InstanceID>0</InstanceID><Channel>Master</Channel>"
                     "<DesiredVolume>%d</DesiredVolume>", arg);
            send_soap("RenderingControl", "SetVolume", vol_args, resp, sizeof(resp));
            if (xSemaphoreTake(s_track_mutex, pdMS_TO_TICKS(20))) {
                s_track.volume = (uint8_t)arg;
                xSemaphoreGive(s_track_mutex);
            }
            break;
        default: break;
    }
}

// ---- Background tasks ----------------------------------------------

// Browse the Sonos ContentDirectory FV:2 container to get upnp:albumArtURI for
// each favourite.  The Sonos player serves art for Spotify/radio via /getaa?...
// which sonos_favourite_art() already resolves to a full URL.
static bool fetch_fav_art(void)
{
    if (s_active_idx < 0) return true;
    const char *ip = s_speakers[s_active_idx].ip;

    static char url[128];
    snprintf(url, sizeof(url),
             "http://%s:1400/MediaServer/ContentDirectory/Control", ip);

    // Fixed Browse body — FV:2 is the Sonos favourites container
    static const char body[] =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
        "<ObjectID>FV:2</ObjectID>"
        "<BrowseFlag>BrowseDirectChildren</BrowseFlag>"
        "<Filter>dc:title,upnp:albumArtURI,res</Filter>"
        "<StartingIndex>0</StartingIndex>"
        "<RequestedCount>100</RequestedCount>"
        "<SortCriteria></SortCriteria>"
        "</u:Browse>"
        "</s:Body></s:Envelope>";

    http_ctx_t ctx = { s_browse_resp, 0, BROWSE_RESP_BUF_SZ };
    s_browse_resp[0] = '\0';

    net_pre_wait("FAV_ART", NET_WAIT_GENERAL);
    if (!xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(6000))) {
        ESP_LOGW(TAG, "fetch_fav_art: mutex timeout");
        return false;   // caller should retry
    }
    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 5000,
        .event_handler = http_event_handler,
        .user_data     = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        g_last_network_end_ms = ms_now();
        xSemaphoreGive(g_network_mutex);
        return true;    // not a transient mutex issue
    }
    esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");
    esp_http_client_set_header(client, "SOAPAction",
        "\"urn:schemas-upnp-org:service:ContentDirectory:1#Browse\"");
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err  = esp_http_client_perform(client);
    int       code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    g_last_network_end_ms = ms_now();
    xSemaphoreGive(g_network_mutex);

    if (err != ESP_OK || code != 200) {
        ESP_LOGW(TAG, "fetch_fav_art: failed err=%d code=%d", (int)err, code);
        return true;    // not a mutex issue; don't retry
    }
    ESP_LOGD(TAG, "fetch_fav_art: resp %d bytes", ctx.len);

    // <Result> contains HTML-entity-encoded DIDL-Lite XML.
    // Move it to the front of the buffer and decode in-place.
    const char *rs = strstr(s_browse_resp, "<Result>");
    if (!rs) { ESP_LOGW(TAG, "fetch_fav_art: no <Result>"); return true; }
    rs += 8;
    const char *re = strstr(rs, "</Result>");
    if (!re) { ESP_LOGW(TAG, "fetch_fav_art: no </Result>"); return true; }
    size_t rlen = (size_t)(re - rs);
    memmove(s_browse_resp, rs, rlen);
    s_browse_resp[rlen] = '\0';
    decode_html_entities(s_browse_resp);  // &amp;amp; → &, &lt; → <, etc.

    // Walk DIDL <item> elements; match dc:title → known favourite, store art URI
    const char *p = s_browse_resp;
    int matched = 0;
    for (;;) {
        const char *item = strstr(p, "<item ");
        if (!item) break;
        const char *end = strstr(item, "</item>");
        if (!end) break;

        char title[64]   = {0};
        char art[256]    = {0};

        const char *ts = strstr(item, "<dc:title>");
        if (ts && ts < end) {
            ts += 10;
            const char *te = strstr(ts, "</dc:title>");
            if (te && te <= end) {
                size_t n = (size_t)(te - ts);
                if (n && n < sizeof(title)) { memcpy(title, ts, n); title[n] = '\0'; }
            }
        }

        const char *as = strstr(item, "<upnp:albumArtURI>");
        if (as && as < end) {
            as += 18;
            const char *ae = strstr(as, "</upnp:albumArtURI>");
            if (ae && ae <= end) {
                size_t n = (size_t)(ae - as);
                if (n && n < sizeof(art)) { memcpy(art, as, n); art[n] = '\0'; }
            }
        }


        // Fix Sonos DIDL single-slash quirk: https:/ → https://
        if (strncmp(art, "https:/", 7) == 0 && art[7] != '/') {
            size_t len = strlen(art);
            if (len + 1 < sizeof(art)) {
                memmove(art + 8, art + 7, len - 7 + 1);
                art[7] = '/';
            }
        }


        if (title[0] && art[0]) {
            for (int i = 0; i < s_fav_count; i++) {
                if (strcasecmp(s_fav_names[i], title) == 0) {
                    strncpy(s_fav_art[i], art, sizeof(s_fav_art[i]) - 1);
                    s_fav_art[i][sizeof(s_fav_art[i]) - 1] = '\0';
                    ESP_LOGI(TAG, "fav_art[%d] %s → %.100s", i, title, art);
                    matched++;
                    break;
                }
            }
        }

        p = end + 7;  // advance past </item>
    }
    ESP_LOGI(TAG, "fetch_fav_art: matched %d/%d", matched, s_fav_count);
    return true;
}

// Runs fetch_favourites() in its own short-lived task so poll_task is never
// blocked by the HTTP call (which can take up to 11 s on a slow/absent server).
static void fetch_favs_task(void *arg)
{
    bool attempted = fetch_favourites();
    if (attempted && s_fav_count > 0) {
        bool art_ok = fetch_fav_art();  // UPnP Browse FV:2 → populates s_fav_art[]
        if (!art_ok)
            s_fetch_favs_requested = true;  // mutex was busy; retry on next poll cycle
    }
    s_fetch_task_running = false;
    if (!attempted) {
        // Mutex was held (e.g. by an art download) — retry on next poll cycle
        s_fetch_favs_requested = true;
    }
    vTaskDelete(NULL);
}

static void trigger_fetch_favs(void)
{
    if (s_fetch_task_running) return;
    s_fetch_task_running = true;
    if (xTaskCreate(fetch_favs_task, "fav_fetch", 6144, NULL, 4, NULL) != pdPASS) {
        s_fetch_task_running = false;
        ESP_LOGW(TAG, "trigger_fetch: out of memory");
    }
}

static void poll_task(void *arg)
{
    ESP_LOGI(TAG, "poll_task: started");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Fire an initial favourites fetch in the background; clear the requested
    // flag so the loop's s_fetch_favs_requested check doesn't double-trigger.
    s_fetch_favs_requested = false;
    trigger_fetch_favs();

    int      cycle         = 0;
    uint32_t last_probe_ms = 0;

    for (;;) {
        // Retry favourites fetch when API server becomes available or every
        // 5 minutes while we still have none.  Runs in a background task so
        // poll_task is never blocked waiting for the HTTP response.
        if (s_fetch_favs_requested ||
            (cycle > 0 && s_fav_count == 0 && g_api_server[0] && cycle % 150 == 0)) {
            s_fetch_favs_requested = false;
            trigger_fetch_favs();
        }

        // WiFi reconnect signal: force immediate reconnect probe
        if (s_need_reconnect) {
            s_need_reconnect = false;
            last_probe_ms    = 0;
            ESP_LOGI(TAG, "WiFi reconnect: probing speaker");
        }

        if (s_active_idx >= 0 && s_speakers[s_active_idx].connected) {
            update_track_info();
            vTaskDelay(pdMS_TO_TICKS(300));
            update_playback_state();
            if (cycle % 5 == 0) {
                vTaskDelay(pdMS_TO_TICKS(300));
                update_volume();
            }
        } else if (s_active_idx >= 0) {
            // Speaker marked disconnected — probe every 30s
            // send_soap inside update_track_info sets connected=true on HTTP 200
            uint32_t now = ms_now();
            if (now - last_probe_ms >= 30000) {
                last_probe_ms = now;
                ESP_LOGI(TAG, "Probe: reconnecting to %s",
                         s_speakers[s_active_idx].ip);
                update_track_info();
            }
        }
        cycle++;
        vTaskDelay(pdMS_TO_TICKS(SONOS_POLL_INTERVAL_MS));
    }
}

static void cmd_task(void *arg)
{
    sonos_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (cmd.type == CMD_PLAY_FAV)
                do_play_favourite(cmd.arg);
            else
                do_soap_cmd(cmd.type, cmd.arg);
        }
    }
}

// ---- node-sonos-http-api server discovery --------------------------
// Ported from ESPSonos NodeSonosServer (C++ → C).
// Probes the /24 subnet for port 5005; verifies with GET /zones.
// Caches result in g_api_server (NVS key: NVS_KEY_API_SERVER).

static volatile bool s_api_scan_running = false;

static bool probe_tcp(const char *ip, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    struct timeval tv = { 0, SONOS_API_PROBE_MS * 1000 };
    bool ok = (select(sock + 1, NULL, &fds, NULL, &tv) > 0);
    close(sock);
    return ok;
}

static bool verify_api_server(const char *ip)
{
    if (!xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(2000))) return false;

    char url[64];
    snprintf(url, sizeof(url), "http://%s:%d/zones", ip, SONOS_API_PORT);

    static char resp[256];
    http_ctx_t ctx = { resp, 0, (int)sizeof(resp) };
    resp[0] = '\0';

    esp_http_client_config_t cfg = {
        .url           = url,
        .timeout_ms    = 2000,
        .event_handler = http_event_handler,
        .user_data     = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err  = esp_http_client_perform(client);
    int       code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    xSemaphoreGive(g_network_mutex);

    // node-sonos-http-api /zones returns a JSON array starting with '['
    bool ok = (err == ESP_OK && code == 200 && resp[0] == '[');
    ESP_LOGI(TAG, "verify_api %s → %s", ip, ok ? "OK" : "FAIL");
    return ok;
}

static void save_api_server(const char *ip)
{
    snprintf(g_api_server, sizeof(g_api_server), "%s:%d", ip, SONOS_API_PORT);
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_API_SERVER, g_api_server);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "API server saved: %s", g_api_server);
    s_fetch_favs_requested = true;  // signal poll_task to fetch favourites immediately
}

static void api_scan_task(void *arg)
{
    const char *my_ip = (const char *)arg;

    // Build /24 prefix (first 3 octets)
    char prefix[20];
    strlcpy(prefix, my_ip, sizeof(prefix));
    char *last_dot = strrchr(prefix, '.');
    if (!last_dot) { s_api_scan_running = false; vTaskDelete(NULL); return; }
    *(last_dot + 1) = '\0';
    int own_octet = atoi(last_dot + 1 - (prefix + strlen(prefix) - last_dot - 1));
    // Recalculate own_octet from original ip string
    own_octet = atoi(strrchr(my_ip, '.') + 1);

    ESP_LOGI(TAG, "API scan: %sx :%d", prefix, SONOS_API_PORT);

    char host[24];
    for (int i = 1; i <= 254; i++) {
        if (i == own_octet) continue;
        snprintf(host, sizeof(host), "%s%d", prefix, i);
        if (probe_tcp(host, SONOS_API_PORT) && verify_api_server(host)) {
            save_api_server(host);
            break;
        }
        if ((i % 16) == 0) vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (!g_api_server[0])
        ESP_LOGW(TAG, "API scan complete — server not found");

    s_api_scan_running = false;
    vTaskDelete(NULL);
}

bool sonos_api_server_init(const char *my_ip)
{
    // Try cached value first
    if (g_api_server[0]) {
        char ip_only[48];
        strlcpy(ip_only, g_api_server, sizeof(ip_only));
        char *colon = strchr(ip_only, ':');
        if (colon) *colon = '\0';
        if (verify_api_server(ip_only)) {
            ESP_LOGI(TAG, "API server cached + reachable: %s", g_api_server);
            return true;
        }
        ESP_LOGW(TAG, "Cached API server %s unreachable — scanning", g_api_server);
        g_api_server[0] = '\0';
    }

    // Start background subnet scan
    if (!s_api_scan_running && my_ip && my_ip[0]) {
        s_api_scan_running = true;
        static char ip_buf[24];
        strlcpy(ip_buf, my_ip, sizeof(ip_buf));
        xTaskCreate(api_scan_task, "api_scan", 4096, ip_buf, 2, NULL);
        ESP_LOGI(TAG, "API server not cached — background scan started");
    }
    return false;
}

bool sonos_api_server_ready(void)
{
    return g_api_server[0] != '\0';
}

// ---- Public API ----------------------------------------------------

void sonos_controller_init(void)
{
    s_track_mutex = xSemaphoreCreateMutex();
    s_cmd_queue   = xQueueCreate(8, sizeof(sonos_cmd_t));
    memset(&s_track, 0, sizeof(s_track));
    spiffs_art_init();
    load_device_favourites();   // also calls load_art_from_spiffs()
    ESP_LOGI(TAG, "Controller initialised");
}

void sonos_controller_start_polling(void)
{
    ESP_LOGI(TAG, "DRAM free before tasks: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (!s_poll_task) {
        BaseType_t r = xTaskCreate(poll_task, "sonos_poll", 8192, NULL, 5, &s_poll_task);
        if (r != pdPASS) {
            ESP_LOGE(TAG, "poll_task create FAILED — out of internal DRAM");
            s_poll_task = NULL;
        }
    }
    if (!s_cmd_task) {
        BaseType_t r = xTaskCreate(cmd_task, "sonos_cmd", 4096, NULL, 6, &s_cmd_task);
        if (r != pdPASS) {
            ESP_LOGE(TAG, "cmd_task create FAILED — out of internal DRAM");
            s_cmd_task = NULL;
        }
    }
    ESP_LOGI(TAG, "Background tasks started");
}

void sonos_on_wifi_ready(void)
{
    s_need_reconnect = true;
    ESP_LOGI(TAG, "WiFi ready: scheduling reconnect probe");
}

static void enqueue(sonos_cmd_type_t type, int arg)
{
    sonos_cmd_t cmd = { type, arg };
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void sonos_play(void)               { enqueue(CMD_PLAY,       0); }
void sonos_pause(void)              { enqueue(CMD_PAUSE,      0); }
void sonos_toggle_play_pause(void)  { enqueue(CMD_TOGGLE,     0); }
void sonos_next(void)               { enqueue(CMD_NEXT,       0); }
void sonos_prev(void)               { enqueue(CMD_PREV,       0); }
void sonos_set_volume(uint8_t v)    { enqueue(CMD_SET_VOLUME, v); }
void sonos_play_favourite(uint8_t i){ enqueue(CMD_PLAY_FAV,   i); }

void sonos_get_track(sonos_track_t *out)
{
    if (!out) return;
    if (xSemaphoreTake(s_track_mutex, pdMS_TO_TICKS(50))) {
        memcpy(out, &s_track, sizeof(*out));
        xSemaphoreGive(s_track_mutex);
    }
}

int sonos_get_speakers(sonos_speaker_t *out, int max_count)
{
    int n = s_speaker_count < max_count ? s_speaker_count : max_count;
    if (out) memcpy(out, s_speakers, n * sizeof(sonos_speaker_t));
    return n;
}

void sonos_select_speaker(int index)
{
    if (index < 0 || index >= s_speaker_count) return;
    s_active_idx = index;
    s_speakers[index].connected = true;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_SPEAKER_IP,   s_speakers[index].ip);
        nvs_set_str(nvs, NVS_KEY_SPEAKER_NAME, s_speakers[index].name);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Active: %s (%s)", s_speakers[index].name, s_speakers[index].ip);
    // Don't create a fetch task here — poll_task hasn't started yet and the
    // fav_fetch stack would hold DRAM until the IDLE task cleans it up,
    // leaving too little for poll_task/cmd_task.  Set the flag instead.
    s_fetch_favs_requested = true;
}

const char *sonos_active_speaker_name(void)
{
    if (s_active_idx < 0) return "";
    return s_speakers[s_active_idx].name;
}

bool sonos_is_connected(void)
{
    if (s_active_idx < 0) return false;
    return s_speakers[s_active_idx].connected;
}

// ---- Merged favourites (Sonos built-ins + device custom) -----------
// Indices 0..s_fav_count-1        → Sonos built-in favourites
// Indices s_fav_count..total-1    → device custom favourites (NVS)

int sonos_favourites_count(void) { return s_fav_count + s_dev_fav_count; }

const char *sonos_favourite_name(int index)
{
    if (index < 0) return "";
    if (index < s_fav_count) return s_fav_names[index];
    int di = index - s_fav_count;
    if (di < s_dev_fav_count) return s_dev_fav_names[di];
    return "";
}

const char *sonos_favourite_art(int index)
{
    if (index < 0 || index >= s_fav_count) return "";
    const char *url = s_fav_art[index];
    if (!url[0]) return "";
    // Absolute URL (https:// or http://) — return directly; prepare_url() handles https→http
    if (url[0] != '/') return url;
    // Relative URL — needs speaker IP
    if (s_active_idx < 0) return "";
    static char resolved[512];
    snprintf(resolved, sizeof(resolved), "http://%s:1400%s",
             s_speakers[s_active_idx].ip, url);
    return resolved;
}

// ---- Device custom favourite management ----------------------------

bool sonos_add_device_favourite(const char *name, const char *cmd)
{
    if (!name || !cmd || s_dev_fav_count >= MAX_DEVICE_FAVOURITES) return false;
    strlcpy(s_dev_fav_names[s_dev_fav_count], name, sizeof(s_dev_fav_names[0]));
    strlcpy(s_dev_fav_cmds[s_dev_fav_count],  cmd,  sizeof(s_dev_fav_cmds[0]));
    s_dev_fav_count++;
    save_device_favourites();
    ESP_LOGI(TAG, "Added device fav: %s → %s", name, cmd);
    return true;
}

bool sonos_remove_device_favourite(int index)
{
    if (index < 0 || index >= s_dev_fav_count) return false;

    // Shift names, cmds, and PSRAM art data down to fill the gap
    for (int i = index; i < s_dev_fav_count - 1; i++) {
        memcpy(s_dev_fav_names[i], s_dev_fav_names[i + 1], sizeof(s_dev_fav_names[0]));
        memcpy(s_dev_fav_cmds[i],  s_dev_fav_cmds[i + 1],  sizeof(s_dev_fav_cmds[0]));
        if (s_dev_fav_art_sz[i + 1] > 0)
            memcpy(s_dev_fav_art_data[i], s_dev_fav_art_data[i + 1], s_dev_fav_art_sz[i + 1]);
        s_dev_fav_art_sz[i] = s_dev_fav_art_sz[i + 1];
    }
    s_dev_fav_count--;
    s_dev_fav_names[s_dev_fav_count][0] = '\0';
    s_dev_fav_cmds[s_dev_fav_count][0]  = '\0';
    s_dev_fav_art_sz[s_dev_fav_count]   = 0;

    // Shift SPIFFS art files to match new indices
    char old_path[32], new_path[32];
    for (int i = index; i < s_dev_fav_count; i++) {
        spiffs_art_path(i + 1, old_path, sizeof(old_path));
        spiffs_art_path(i,     new_path, sizeof(new_path));
        rename(old_path, new_path);
    }
    // Delete the now-orphaned last slot file (if any)
    spiffs_art_path(s_dev_fav_count, old_path, sizeof(old_path));
    unlink(old_path);

    save_device_favourites();
    ESP_LOGI(TAG, "Removed device fav[%d]", index);
    return true;
}

int         sonos_device_fav_count(void)       { return s_dev_fav_count; }
const char *sonos_device_fav_name(int index)   { return (index >= 0 && index < s_dev_fav_count) ? s_dev_fav_names[index] : ""; }
const char *sonos_device_fav_cmd(int index)    { return (index >= 0 && index < s_dev_fav_count) ? s_dev_fav_cmds[index]  : ""; }

bool sonos_set_device_fav_art(int idx, const uint8_t *jpeg, size_t sz)
{
    if (idx < 0 || idx >= s_dev_fav_count || !jpeg || sz == 0) return false;
    if (sz > ART_MAX_BYTES) sz = ART_MAX_BYTES;
    memcpy(s_dev_fav_art_data[idx], jpeg, sz);
    s_dev_fav_art_sz[idx] = sz;
    char path[32];
    spiffs_art_path(idx, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "Cannot write art[%d]: %s", idx, path); return false; }
    fwrite(jpeg, 1, sz, f);
    fclose(f);
    ESP_LOGI(TAG, "Art[%d] saved: %zu bytes", idx, sz);
    return true;
}

const uint8_t *sonos_device_fav_art_data(int idx)
{
    if (idx < 0 || idx >= s_dev_fav_count || s_dev_fav_art_sz[idx] == 0) return NULL;
    return s_dev_fav_art_data[idx];
}

size_t sonos_device_fav_art_size(int idx)
{
    if (idx < 0 || idx >= s_dev_fav_count) return 0;
    return s_dev_fav_art_sz[idx];
}

void sonos_play_device_favourite(int idx)
{
    if (idx < 0 || idx >= s_dev_fav_count) return;
    enqueue(CMD_PLAY_FAV, s_fav_count + idx);
}
