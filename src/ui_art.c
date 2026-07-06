/**
 * Album art: download → TJpgDec → bilinear scale → PSRAM RGB565 buffer.
 * Ported from ESPSonos ui_album_art.cpp; adapted for ESP-IDF C + ESP32-S3.
 *
 * Key differences from ESPSonos:
 *   - No ESP32-P4 HW JPEG decoder; TJpgDec (bundled in LVGL) used exclusively.
 *   - No SDIO/SO_RCVBUF machinery; standard WiFi + esp_http_client.
 *   - ART_SIZE=300 for 480×480 round display (fits inside progress arc).
 *   - Double-buffer scheme prevents overwrite of buffer LVGL is reading from.
 */

#include "ui_art.h"
#include "globals.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
// TJpgDec is compiled into the LVGL library; its src/ dir is in the include path.
#include "extra/libs/sjpg/tjpgd.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "ui_art";

// ── Sizes ─────────────────────────────────────────────────────────────────────
#define ART_PIXEL_BUF_BYTES  (ART_SIZE * ART_SIZE * sizeof(lv_color_t)) // 180 KB
#define ART_DOWNLOAD_BYTES   (280 * 1024)   // 280 KB — covers Spotify 640×640
#define ART_TJPGD_POOL_BYTES 4096           // TJpgDec work pool (per LVGL recommendation)
#define ART_TASK_STACK       8192
#define ART_CACHE_SLOTS      2
#define ART_CONSUMED_TIMEOUT_MS 6000        // max wait for LVGL to consume new art

// ── LRU cache ─────────────────────────────────────────────────────────────────
typedef struct {
    char      url[512];
    uint16_t *pixels;   // ART_SIZE×ART_SIZE RGB565, PSRAM
    uint32_t  dominant;
    bool      valid;
} art_cache_t;

static art_cache_t s_cache[ART_CACHE_SLOTS];
static int         s_cache_lru = 0;

// ── Double-buffer display state ───────────────────────────────────────────────
// art_task writes to s_work_buf, swaps with s_disp_buf, then sets s_new_art.
// LVGL task reads from s_disp_buf via s_art_dsc.  They never alias.
static uint16_t    *s_disp_buf = NULL;
static uint16_t    *s_work_buf = NULL;
static lv_img_dsc_t s_art_dsc;

static volatile bool     s_new_art  = false;
static volatile uint32_t s_dominant = 0x1a1a1a;

// ── URL / blob signalling (art_task ↔ caller) ─────────────────────────────────
static SemaphoreHandle_t s_url_mutex    = NULL;
static char              s_pending[512] = {0};
static char              s_current[512] = {0};
// Blob path: set by ui_art_request_blob(); art_task consumes it once.
static const uint8_t    *s_blob_data    = NULL;
static size_t            s_blob_size    = 0;

// Dedicated display buffer for blob (favourites) art.
// Kept separate from the URL double-buffer so the two screens can't
// overwrite each other's LVGL image descriptor.
static uint16_t     *s_blob_buf      = NULL;
static lv_img_dsc_t  s_blob_art_dsc  = {0};
static volatile bool s_new_blob_art  = false;

// TJpgDec work pool — 4 KB, lives in IRAM (fast access needed during MCU decode).
static uint8_t s_tjpgd_pool[ART_TJPGD_POOL_BYTES] __attribute__((aligned(4)));

// ── TJpgDec context ───────────────────────────────────────────────────────────
typedef struct {
    // input (infunc)
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    // output (outfunc)
    lv_color_t    *buf;
    int            img_w;
    int            img_h;
} jpg_ctx_t;

static size_t jpg_infunc(JDEC *jd, uint8_t *dst, size_t nbytes)
{
    jpg_ctx_t *ctx = (jpg_ctx_t *)jd->device;
    size_t avail = ctx->len - ctx->pos;
    if (!avail) return 0;
    size_t n = (nbytes < avail) ? nbytes : avail;
    if (dst) memcpy(dst, ctx->data + ctx->pos, n);
    ctx->pos += n;
    return n;
}

// JD_FORMAT=0 → bitmap is RGB888, one MCU block per call.
static int jpg_outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpg_ctx_t     *ctx = (jpg_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    int blk_w = rect->right  - rect->left + 1;
    int blk_h = rect->bottom - rect->top  + 1;

    for (int y = 0; y < blk_h; y++) {
        int oy = rect->top + y;
        if (oy >= ctx->img_h) continue;
        for (int x = 0; x < blk_w; x++) {
            int ox = rect->left + x;
            if (ox >= ctx->img_w) continue;
            const uint8_t *p = src + (y * blk_w + x) * 3;
            ctx->buf[oy * ctx->img_w + ox] = lv_color_make(p[0], p[1], p[2]);
        }
    }
    return 1;
}

// Decode raw JPEG bytes → PSRAM lv_color_t buffer.  Caller must heap_caps_free().
static lv_color_t *decode_jpeg(const uint8_t *data, size_t len, int *out_w, int *out_h)
{
    jpg_ctx_t ctx = { .data = data, .len = len, .pos = 0,
                      .buf = NULL, .img_w = 0, .img_h = 0 };
    JDEC jd;
    JRESULT r = jd_prepare(&jd, jpg_infunc, s_tjpgd_pool, sizeof(s_tjpgd_pool), &ctx);
    if (r != JDR_OK) {
        ESP_LOGW(TAG, "jd_prepare failed: %d", (int)r);
        return NULL;
    }

    int w = (int)jd.width, h = (int)jd.height;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        ESP_LOGW(TAG, "JPEG dims invalid: %dx%d", w, h);
        return NULL;
    }

    size_t buf_bytes = (size_t)w * h * sizeof(lv_color_t);
    lv_color_t *buf = (lv_color_t *)heap_caps_malloc(buf_bytes,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "decode buf alloc failed (%u B)", (unsigned)buf_bytes);
        return NULL;
    }

    ctx.buf   = buf;
    ctx.img_w = w;
    ctx.img_h = h;

    r = jd_decomp(&jd, jpg_outfunc, 0 /* scale=1:1 */);
    if (r != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp failed: %d", (int)r);
        heap_caps_free(buf);
        return NULL;
    }

    *out_w = w;
    *out_h = h;
    ESP_LOGI(TAG, "decoded %dx%d (%u KB)", w, h, (unsigned)(buf_bytes / 1024));
    return buf;
}

// ── Bilinear scale (ported from ESPSonos scaleImageBilinear) ─────────────────
// src_stride: row pitch in pixels (for HW-padded output; equals src_w here).
static void scale_bilinear(const uint16_t *src, int src_w, int src_h, int src_stride,
                            uint16_t *dst, int dst_w, int dst_h)
{
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;

    int xr = (int)(((int64_t)(src_w - 1) << 16) / dst_w);
    int yr = (int)(((int64_t)(src_h - 1) << 16) / dst_h);

    for (int dy = 0; dy < dst_h; dy++) {
        int   syf = dy * yr;
        int   y0  = syf >> 16;
        int   y1  = (y0 + 1 < src_h) ? y0 + 1 : src_h - 1;
        int   yw  = (syf >> 8) & 0xFF;

        const uint16_t *r0 = src + y0 * src_stride;
        const uint16_t *r1 = src + y1 * src_stride;
        uint16_t *drow = dst + dy * dst_w;

        for (int dx = 0; dx < dst_w; dx++) {
            int sxf = dx * xr;
            int x0  = sxf >> 16;
            int x1  = (x0 + 1 < src_w) ? x0 + 1 : src_w - 1;
            int xw  = (sxf >> 8) & 0xFF;

            uint16_t p00 = r0[x0], p10 = r0[x1];
            uint16_t p01 = r1[x0], p11 = r1[x1];

            int r00=(p00>>11)&0x1F, g00=(p00>>5)&0x3F, b00=p00&0x1F;
            int r10=(p10>>11)&0x1F, g10=(p10>>5)&0x3F, b10=p10&0x1F;
            int r01=(p01>>11)&0x1F, g01=(p01>>5)&0x3F, b01=p01&0x1F;
            int r11=(p11>>11)&0x1F, g11=(p11>>5)&0x3F, b11=p11&0x1F;

            int nxw = 256 - xw, nyw = 256 - yw;
            uint8_t r = (uint8_t)(((r00*nxw + r10*xw)*nyw + (r01*nxw + r11*xw)*yw) >> 16);
            uint8_t g = (uint8_t)(((g00*nxw + g10*xw)*nyw + (g01*nxw + g11*xw)*yw) >> 16);
            uint8_t b = (uint8_t)(((b00*nxw + b10*xw)*nyw + (b01*nxw + b11*xw)*yw) >> 16);

            drow[dx] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

// ── Dominant colour (ported from ESPSonos sampleDominantColor) ────────────────
static uint32_t sample_dominant_color(const uint16_t *px, int w, int h)
{
    uint32_t rs = 0, gs = 0, bs = 0;
    int      cnt = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (((x | y) % 20 == 0) && (y < 50 || y > h-50 || x < 50 || x > w-50)) {
                uint16_t p = px[y * w + x];
                rs += (p >> 8) & 0xF8;
                gs += (p >> 3) & 0xFC;
                bs += (p << 3) & 0xF8;
                cnt++;
            }
        }
    }
    if (!cnt) return 0x1a1a1a;
    // Darken average to 40% — same formula as ESPSonos
    uint8_t r = (uint8_t)((rs / cnt * 4) / 10);
    uint8_t g = (uint8_t)((gs / cnt * 4) / 10);
    uint8_t b = (uint8_t)((bs / cnt * 4) / 10);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// ── HTML entity decode ────────────────────────────────────────────────────────
static void decode_html_entities(const char *in, char *out, size_t sz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < sz; ) {
        if      (!strncmp(in+i,"&amp;", 5)) { out[o++]='&'; i+=5; }
        else if (!strncmp(in+i,"&lt;",  4)) { out[o++]='<'; i+=4; }
        else if (!strncmp(in+i,"&gt;",  4)) { out[o++]='>'; i+=4; }
        else if (!strncmp(in+i,"&quot;",6)) { out[o++]='"'; i+=6; }
        else                                { out[o++]=in[i++];    }
    }
    out[o] = '\0';
}

// Prepare URL: entity decode, HTTPS→HTTP, Sonos getaa sizing, Deezer resize.
static void prepare_url(const char *raw, char *out, size_t sz)
{
    decode_html_entities(raw, out, sz);

    // Fix Sonos DIDL single-slash quirk before conversion: https:/ → https://
    if (!strncmp(out, "https:/", 7) && out[7] != '/') {
        size_t len = strlen(out);
        if (len + 1 < sz) {
            memmove(out + 8, out + 7, len - 7 + 1);
            out[7] = '/';
        }
    }

    // sali.sonos.radio is a redirect-only proxy to cdn-profiles.tunein.com.
    // Extract the actual CDN URL from the percent-encoded "image=" parameter.
    if (strstr(out, "sali.sonos.radio/")) {
        char *img = strstr(out, "image=");
        if (img) {
            img += 6;  // skip "image="
            char decoded[512];
            size_t o = 0;
            for (const char *p = img; *p && *p != '&' && o + 1 < sizeof(decoded); ) {
                if (p[0] == '%' && p[1] && p[2]) {
                    int hi = -1, lo = -1;
                    char c1 = p[1], c2 = p[2];
                    if      (c1 >= '0' && c1 <= '9') hi = c1 - '0';
                    else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
                    else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
                    if      (c2 >= '0' && c2 <= '9') lo = c2 - '0';
                    else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
                    else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
                    if (hi >= 0 && lo >= 0) {
                        decoded[o++] = (char)((hi << 4) | lo);
                        p += 3;
                        continue;
                    }
                }
                decoded[o++] = *p++;
            }
            decoded[o] = '\0';
            if (o > 8) strlcpy(out, decoded, sz);
        }
    }

    // HTTPS → HTTP for local Sonos speaker (:1400/) and TuneIn CDN.
    // cdn-profiles.tunein.com serves JPEG over plain HTTP; avoids TLS version issues.
    if (!strncmp(out, "https://", 8) &&
        (strstr(out, ":1400/") || strstr(out, "cdn-profiles.tunein.com"))) {
        memmove(out + 4, out + 5, strlen(out + 5) + 1);   // https:// → http://
    }

    // Sonos getaa: request art at display size (300×300 art area) — 4× less data than 600×600
    if (strstr(out, "/getaa?") && !strstr(out, "maxWidth")) {
        size_t cur = strlen(out);
        if (cur + 26 < sz)
            strncat(out, "&maxWidth=300&maxHeight=300", sz - cur - 1);
    }

    // Deezer: reduce 1000×1000 to 400×400
    char *dz = strstr(out, "/1000x1000-");
    if (dz && strstr(out, "dzcdn.net")) {
        memmove(dz + 9, dz + 11, strlen(dz + 11) + 1);
        memcpy(dz, "/400x400-", 9);
    }

    // Spotify CDN (spotifycdn.com) serves WebP by default.
    // Append .jpeg as a format hint — some CDN configs honour the extension.
    // Do NOT rewrite to i.scdn.co: Spotify's branded domain is GFW-blocked in China.
    {
        char *pos = strstr(out, "spotifycdn.com/image/");
        if (pos) {
            size_t len = strlen(out);
            const char *after_slash = strrchr(pos, '/');
            if (after_slash && !strchr(after_slash, '.') && !strchr(after_slash, '?')) {
                if (len + 5 < sz)
                    strlcat(out, ".jpeg", sz);
            }
        }
    }

}

// ── HTTP/HTTPS download ───────────────────────────────────────────────────────
typedef struct { uint8_t *buf; size_t max; size_t total; } art_dl_ctx_t;

static esp_err_t art_dl_evt(esp_http_client_event_t *evt)
{
    art_dl_ctx_t *ctx = (art_dl_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_REDIRECT) {
        ctx->total = 0;   // discard any partial body before redirect
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && ctx->total < ctx->max) {
        size_t space = ctx->max - ctx->total;
        size_t copy  = (size_t)evt->data_len < space ? (size_t)evt->data_len : space;
        memcpy(ctx->buf + ctx->total, evt->data, copy);
        ctx->total += copy;
    }
    return ESP_OK;
}

static size_t download_url(const char *url, uint8_t *buf, size_t buf_sz)
{
    art_dl_ctx_t ctx = { .buf = buf, .max = buf_sz - 1, .total = 0 };
    esp_http_client_config_t cfg = {
        .url                         = url,
        .timeout_ms                  = 8000,
        .buffer_size                 = 4096,
        .max_redirection_count       = 3,
        .crt_bundle_attach           = esp_crt_bundle_attach,  // Mozilla CA bundle
        .skip_cert_common_name_check = true,   // skip CN check for CDN flexibility
        .event_handler               = art_dl_evt,
        .user_data                   = &ctx,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return 0;

    // JPEG/PNG only — no wildcard fallback so CDNs don't serve WebP
    esp_http_client_set_header(c, "Accept", "image/jpeg, image/png");
    // TuneIn CDN hotlink protection: requires a Referer header
    if (strstr(url, "cdn-profiles.tunein.com"))
        esp_http_client_set_header(c, "Referer", "https://tunein.com/");
    esp_err_t err    = esp_http_client_perform(c);
    int       status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "download err=%d status=%d", (int)err, status);
        return 0;
    }
    return ctx.total;
}

// ── Background art task ───────────────────────────────────────────────────────
static void art_task(void *arg)
{
    // Allocate 280 KB download buffer in PSRAM (too large for internal DRAM)
    uint8_t *dl_buf = (uint8_t *)heap_caps_malloc(ART_DOWNLOAD_BYTES,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dl_buf) {
        ESP_LOGE(TAG, "dl_buf alloc failed — art disabled");
        vTaskDelete(NULL);
        return;
    }

    // Allocate LRU cache pixel buffers
    for (int i = 0; i < ART_CACHE_SLOTS; i++) {
        s_cache[i].pixels = (uint16_t *)heap_caps_malloc(ART_PIXEL_BUF_BYTES,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_cache[i].pixels)
            ESP_LOGE(TAG, "cache[%d] alloc failed", i);
    }

    static char url[512];

    while (1) {
        // ── Check for work: blob takes priority over URL ──────────────────────
        const uint8_t *blob    = NULL;
        size_t         blob_sz = 0;
        bool           has_work;

        xSemaphoreTake(s_url_mutex, portMAX_DELAY);
        if (s_blob_data && s_blob_size > 0) {
            blob        = s_blob_data;
            blob_sz     = s_blob_size;
            s_blob_data = NULL;
            s_blob_size = 0;
            has_work    = true;
            url[0]      = '\0';
        } else {
            has_work = (s_pending[0] != '\0' &&
                        strcmp(s_pending, s_current) != 0);
            if (has_work) {
                prepare_url(s_pending, url, sizeof(url));
                strncpy(s_current, s_pending, sizeof(s_current) - 1);
            }
        }
        xSemaphoreGive(s_url_mutex);

        if (!has_work) { vTaskDelay(pdMS_TO_TICKS(300)); continue; }

        // ── Determine source: blob (no download) or URL ───────────────────────
        const uint8_t *src_data;
        size_t         src_len;

        if (blob) {
            src_data = blob;
            src_len  = blob_sz;
            ESP_LOGI(TAG, "blob art: %zu bytes", src_len);
        } else {
            ESP_LOGI(TAG, "requesting: %s", url);

            // LRU cache check
            bool cache_hit = false;
            for (int i = 0; i < ART_CACHE_SLOTS; i++) {
                if (s_cache[i].valid && s_cache[i].pixels &&
                    strncmp(s_cache[i].url, url, sizeof(s_cache[i].url)) == 0) {
                    ESP_LOGI(TAG, "cache hit slot %d", i);
                    memcpy(s_work_buf, s_cache[i].pixels, ART_PIXEL_BUF_BYTES);
                    uint32_t dom = s_cache[i].dominant;
                    s_cache_lru = i;
                    cache_hit = true;
                    uint16_t *tmp = s_disp_buf; s_disp_buf = s_work_buf; s_work_buf = tmp;
                    s_dominant = dom;
                    s_new_art  = true;
                    uint32_t t0 = xTaskGetTickCount();
                    while (s_new_art && (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(ART_CONSUMED_TIMEOUT_MS))
                        vTaskDelay(pdMS_TO_TICKS(100));
                    s_new_art = false;
                    break;
                }
            }
            if (cache_hit) continue;

            // Download
            size_t dl_len = 0;
            if (xSemaphoreTake(g_network_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
                dl_len = download_url(url, dl_buf, ART_DOWNLOAD_BYTES);
                xSemaphoreGive(g_network_mutex);
            } else {
                ESP_LOGW(TAG, "network mutex timeout");
                xSemaphoreTake(s_url_mutex, portMAX_DELAY);
                s_current[0] = '\0';
                xSemaphoreGive(s_url_mutex);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (dl_len < 3) {
                ESP_LOGW(TAG, "download failed or empty");
                xSemaphoreTake(s_url_mutex, portMAX_DELAY);
                s_current[0] = '\0';
                xSemaphoreGive(s_url_mutex);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            src_data = dl_buf;
            src_len  = dl_len;
        }

        // Verify JPEG magic (\xFF\xD8)
        if (src_len < 3 || src_data[0] != 0xFF || src_data[1] != 0xD8) {
            ESP_LOGW(TAG, "not JPEG (got %02x %02x)", src_data[0], src_data[1]);
            continue;
        }

        // ── Decode ────────────────────────────────────────────────────────────
        int iw = 0, ih = 0;
        lv_color_t *decoded = decode_jpeg(src_data, src_len, &iw, &ih);
        if (!decoded) continue;

        // ── Scale into work buffer ─────────────────────────────────────────────
        scale_bilinear((const uint16_t *)decoded, iw, ih, iw,
                       s_work_buf, ART_SIZE, ART_SIZE);
        heap_caps_free(decoded);
        ESP_LOGI(TAG, "scaled %dx%d → %dx%d", iw, ih, ART_SIZE, ART_SIZE);

        // ── Dominant colour ───────────────────────────────────────────────────
        uint32_t dom = sample_dominant_color(s_work_buf, ART_SIZE, ART_SIZE);

        // ── Update LRU cache (URL requests only; blobs have their own PSRAM storage) ──
        if (!blob) {
            int slot = 1 - s_cache_lru;
            if (s_cache[slot].pixels) {
                memcpy(s_cache[slot].pixels, s_work_buf, ART_PIXEL_BUF_BYTES);
                strncpy(s_cache[slot].url, url, sizeof(s_cache[slot].url) - 1);
                s_cache[slot].url[sizeof(s_cache[slot].url) - 1] = '\0';
                s_cache[slot].dominant = dom;
                s_cache[slot].valid    = true;
                s_cache_lru = slot;
            }
        }

        // ── Signal LVGL (separate paths to prevent descriptor aliasing) ──────
        s_dominant = dom;
        if (blob) {
            // Copy to dedicated blob buf — s_art_dsc (URL path) is untouched.
            if (s_blob_buf) memcpy(s_blob_buf, s_work_buf, ART_PIXEL_BUF_BYTES);
            s_new_blob_art = true;
            uint32_t t0 = xTaskGetTickCount();
            while (s_new_blob_art && (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(ART_CONSUMED_TIMEOUT_MS))
                vTaskDelay(pdMS_TO_TICKS(100));
            s_new_blob_art = false;
        } else {
            uint16_t *tmp = s_disp_buf; s_disp_buf = s_work_buf; s_work_buf = tmp;
            s_new_art = true;
            uint32_t t0 = xTaskGetTickCount();
            while (s_new_art && (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(ART_CONSUMED_TIMEOUT_MS))
                vTaskDelay(pdMS_TO_TICKS(100));
            s_new_art = false;
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void ui_art_init(void)
{
    s_url_mutex = xSemaphoreCreateMutex();

    // Allocate double-buffers in PSRAM
    s_disp_buf = (uint16_t *)heap_caps_calloc(ART_PIXEL_BUF_BYTES, 1,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_work_buf = (uint16_t *)heap_caps_calloc(ART_PIXEL_BUF_BYTES, 1,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_blob_buf = (uint16_t *)heap_caps_calloc(ART_PIXEL_BUF_BYTES, 1,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_disp_buf || !s_work_buf || !s_blob_buf)
        ESP_LOGE(TAG, "art buf alloc failed");

    // Seed URL descriptor — data pointer filled in on first ui_art_update()
    memset(&s_art_dsc, 0, sizeof(s_art_dsc));
    s_art_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_art_dsc.header.w  = ART_SIZE;
    s_art_dsc.header.h  = ART_SIZE;
    s_art_dsc.data_size = ART_PIXEL_BUF_BYTES;
    s_art_dsc.data      = (const uint8_t *)s_disp_buf;

    // Seed blob descriptor — points to its own dedicated buffer permanently
    memset(&s_blob_art_dsc, 0, sizeof(s_blob_art_dsc));
    s_blob_art_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_blob_art_dsc.header.w  = ART_SIZE;
    s_blob_art_dsc.header.h  = ART_SIZE;
    s_blob_art_dsc.data_size = ART_PIXEL_BUF_BYTES;
    s_blob_art_dsc.data      = (const uint8_t *)s_blob_buf;

    // Pin art task to core 1 (LVGL runs on core 0) to avoid blocking the UI.
    xTaskCreatePinnedToCore(art_task, "ui_art", ART_TASK_STACK, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "init OK  disp=%p work=%p blob=%p",
             (void*)s_disp_buf, (void*)s_work_buf, (void*)s_blob_buf);
}

void ui_art_request(const char *raw_url)
{
    if (!s_url_mutex) return;
    xSemaphoreTake(s_url_mutex, portMAX_DELAY);
    if (raw_url && raw_url[0])
        strncpy(s_pending, raw_url, sizeof(s_pending) - 1);
    else
        s_pending[0] = '\0';
    xSemaphoreGive(s_url_mutex);
}

void ui_art_request_blob(const uint8_t *jpeg, size_t sz)
{
    if (!s_url_mutex || !jpeg || sz < 3) return;
    xSemaphoreTake(s_url_mutex, portMAX_DELAY);
    s_blob_data = jpeg;
    s_blob_size = sz;
    s_pending[0] = '\0';   // cancel any queued URL; blob takes priority
    xSemaphoreGive(s_url_mutex);
}

bool ui_art_update(lv_obj_t *img_obj)
{
    if (!s_new_art || !img_obj) return false;
    s_new_art = false;

    s_art_dsc.data = (const uint8_t *)s_disp_buf;
    lv_img_set_src(img_obj, &s_art_dsc);
    return true;
}

bool ui_art_update_blob(lv_obj_t *img_obj)
{
    if (!s_new_blob_art || !img_obj) return false;
    s_new_blob_art = false;
    lv_img_set_src(img_obj, &s_blob_art_dsc);
    return true;
}

uint32_t ui_art_dominant_color(void)
{
    return s_dominant;
}
