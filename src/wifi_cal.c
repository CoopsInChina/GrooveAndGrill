#include "wifi_cal.h"
#include "calibration.h"
#include "app_config.h"
#include "wifi_config.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_cal";

// ============================================================
// Calibration page  (GET /)
// ============================================================

static const char CAL_HTML[] =
    "<!DOCTYPE html><html>"
    "<head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Speedo</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:440px;margin:40px auto;padding:0 20px;background:#111;color:#eee}"
    "h2{color:#0af;margin-bottom:4px}"
    "nav a{color:#0af;margin-right:16px;text-decoration:none}"
    "label{display:block;margin-top:16px;color:#aaa;font-size:.9em}"
    "input[type=range]{width:100%;margin:8px 0}"
    ".val{font-size:2.2em;text-align:center;font-weight:bold;color:#0af}"
    "button{width:100%;padding:12px;font-size:1em;"
    "background:#0af;border:0;border-radius:6px;cursor:pointer;color:#000;margin-top:12px}"
    "p.note{color:#666;font-size:.8em}"
    "#toast{position:fixed;bottom:40px;left:50%%;transform:translateX(-50%%);"
    "background:#1a7a3a;color:#fff;padding:14px 32px;border-radius:8px;"
    "font-size:1em;font-weight:bold;opacity:0;transition:opacity .35s;"
    "pointer-events:none;white-space:nowrap}"
    "#toast.show{opacity:1}"
    "</style></head><body>"
    "<h2>Speedometer</h2>"
    "<nav><a href='/'>Calibration</a><a href='/update'>OTA Update</a></nav><hr>"
    "<p>Saved offset: <b id='sv'>%.2f%%</b></p>"
    "<p class='note'>Negative = display reads lower &nbsp;/&nbsp; Positive = display reads higher</p>"
    "<label>Offset %%</label>"
    "<input type='range' id='sl' min='%.0f' max='%.0f' step='0.1' value='%.2f'"
    "  oninput=\"document.getElementById('v').textContent=parseFloat(this.value).toFixed(1)+'%%'\">"
    "<div class='val' id='v'>%.2f%%</div>"
    "<button onclick='save()'>Save calibration</button>"
    "<div id='toast'>&#10003;&nbsp; Calibration saved</div>"
    "<script>"
    "function save(){"
    "  var v=document.getElementById('sl').value;"
    "  fetch('/calibrate',{method:'POST',"
    "    headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "    body:'offset='+encodeURIComponent(v)})"
    "  .then(function(r){"
    "    if(!r.ok)throw new Error(r.status);"
    "    document.getElementById('sv').textContent=parseFloat(v).toFixed(2)+'%%';"
    "    var t=document.getElementById('toast');"
    "    t.classList.add('show');"
    "    setTimeout(function(){t.classList.remove('show');},2500);"
    "  }).catch(function(e){alert('Save failed: '+e);});}"
    "</script>"
    "</body></html>";

static esp_err_t cal_get_handler(httpd_req_t *req)
{
    float pct = cal_get_offset_pct();
    size_t buf_len = sizeof(CAL_HTML) + 64;
    char *buf = malloc(buf_len);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    snprintf(buf, buf_len, CAL_HTML,
             pct,
             -CAL_MAX_OFFSET_PCT, CAL_MAX_OFFSET_PCT,
             pct, pct);
    httpd_resp_set_type(req, "text/html");
    esp_err_t ret = httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ret;
}

static esp_err_t cal_post_handler(httpd_req_t *req)
{
    char body[64] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n > 0) {
        char *p = strstr(body, "offset=");
        if (p) {
            float pct = 0.0f;
            if (sscanf(p + 7, "%f", &pct) == 1) {
                cal_set_offset_pct(pct);
            }
        }
    }
    return httpd_resp_sendstr(req, "ok");
}

// ============================================================
// OTA update page  (GET /update  +  POST /update)
//
// The page uses the Fetch API to POST the raw binary directly
// as application/octet-stream — no multipart parsing needed.
// ============================================================

static const char OTA_HTML[] =
    "<!DOCTYPE html><html>"
    "<head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Speedo OTA</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:440px;margin:40px auto;padding:0 20px;background:#111;color:#eee}"
    "h2{color:#0af}"
    "nav a{color:#0af;margin-right:16px;text-decoration:none}"
    "input[type=file]{display:block;margin:16px 0;color:#eee}"
    "button{width:100%;padding:12px;font-size:1em;background:#0af;border:0;border-radius:6px;cursor:pointer;color:#000}"
    "#status{margin-top:16px;padding:10px;border-radius:4px;display:none}"
    ".ok{background:#1a4;color:#fff}.err{background:#811;color:#fff}.prog{background:#333;color:#0af}"
    "</style></head><body>"
    "<h2>Speedometer</h2>"
    "<nav><a href='/'>Calibration</a><a href='/update'>OTA Update</a></nav><hr>"
    "<h3>Firmware Update</h3>"
    "<p>Select the <code>.bin</code> file built by PlatformIO "
    "(<code>.pio/build/speedo/firmware.bin</code>).</p>"
    "<input type='file' id='f' accept='.bin'>"
    "<button onclick='doFlash()'>Flash firmware</button>"
    "<div id='status'></div>"
    "<script>"
    "function doFlash(){"
    "  var f=document.getElementById('f').files[0];"
    "  if(!f){alert('Select a .bin file first');return;}"
    "  var s=document.getElementById('status');"
    "  s.className='prog';s.style.display='block';"
    "  s.textContent='Uploading '+f.name+' ('+Math.round(f.size/1024)+' KB)...';"
    "  fetch('/update',{method:'POST',body:f,headers:{'Content-Type':'application/octet-stream'}})"
    "  .then(r=>r.text()).then(t=>{"
    "    s.className='ok';s.textContent=t;"
    "  }).catch(e=>{"
    "    s.className='err';s.textContent='Error: '+e;"
    "  });}"
    "</script>"
    "</body></html>";

static esp_err_t ota_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, OTA_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition found — check partitions.csv");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: writing to partition \"%s\" at 0x%08lx",
             target->label, (unsigned long)target->address);

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return err;
    }

    char buf[1024];
    int received;
    bool write_err = false;

    while ((received = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        err = esp_ota_write(ota, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            write_err = true;
            break;
        }
    }

    if (write_err || esp_ota_end(ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA write/verify failed — image may be corrupt");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_ota_set_boot_partition(target));
    ESP_LOGI(TAG, "OTA complete — rebooting");

    httpd_resp_sendstr(req, "Update successful. Device is rebooting...");

    // Small delay so the HTTP response is sent before the reboot
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ============================================================
// WiFi SoftAP + HTTP server init
// ============================================================

esp_err_t wifi_cal_server_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = CAL_WIFI_SSID,
            .ssid_len       = sizeof(CAL_WIFI_SSID) - 1,
            .password       = CAL_WIFI_PASS,
            .channel        = 6,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .max_connection = 2,
        },
    };
    if (strlen(CAL_WIFI_PASS) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started — SSID:\"%s\"  http://192.168.4.1", CAL_WIFI_SSID);

    httpd_config_t srv_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server  = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &srv_cfg));

    static const httpd_uri_t routes[] = {
        { .uri = "/",         .method = HTTP_GET,  .handler = cal_get_handler  },
        { .uri = "/calibrate",.method = HTTP_POST, .handler = cal_post_handler },
        { .uri = "/update",   .method = HTTP_GET,  .handler = ota_get_handler  },
        { .uri = "/update",   .method = HTTP_POST, .handler = ota_post_handler },
    };
    for (int i = 0; i < 4; i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    return ESP_OK;
}
