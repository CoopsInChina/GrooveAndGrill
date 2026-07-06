#include "wifi_manager.h"
#include "sonos_controller.h"
#include "app_config.h"
#include "globals.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t  s_wifi_events  = NULL;
static bool                s_connected    = false;
static bool                s_ap_active    = false;
static char                s_ssid[64]     = {0};
static char                s_ip[20]       = {0};
static httpd_handle_t      s_portal_srv   = NULL;
static esp_netif_t        *s_sta_netif    = NULL;
static esp_netif_t        *s_ap_netif     = NULL;

// ---- Event handler --------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        memset(s_ip, 0, sizeof(s_ip));
        xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        ESP_LOGW(TAG, "STA disconnected");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip);
        s_connected = true;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

// ---- Internal: load creds from NVS ----------------------------------

static bool load_credentials(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t len = ssid_sz;
    nvs_get_str(nvs, NVS_KEY_WIFI_SSID, ssid, &len);
    len = pass_sz;
    nvs_get_str(nvs, NVS_KEY_WIFI_PASS, pass, &len);
    nvs_close(nvs);
    return strlen(ssid) > 0;
}

// ---- Lifecycle -------------------------------------------------------

void wifi_manager_init(void)
{
    if (s_wifi_events) return;  // already initialised
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(ret);

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // Keep radio always active — no modem sleep on mains-powered device
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool wifi_manager_begin_connect(void)
{
    char ssid[64] = {0}, pass[64] = {0};
    if (!load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "No saved credentials");
        return false;
    }

    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);

    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "Connecting to '%s' (background)...", ssid);
    return true;
}

bool wifi_manager_wait_connect(uint32_t timeout_ms)
{
    // Per SonosESP pattern: 3 retries × 10s each, with disconnect+reconnect between attempts
    char ssid[64] = {0}, pass[64] = {0};
    load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    for (int retry = 0; retry < WIFI_CONNECT_RETRIES; retry++) {
        if (retry > 0) {
            ESP_LOGI(TAG, "Retry %d/%d for '%s'", retry, WIFI_CONNECT_RETRIES - 1, ssid);
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));

            wifi_config_t wcfg = {0};
            strncpy((char *)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid) - 1);
            strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);
            esp_wifi_set_config(WIFI_IF_STA, &wcfg);
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
            esp_wifi_connect();
        }

        EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(10000));

        if (bits & WIFI_CONNECTED_BIT) {
            strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
            ESP_LOGI(TAG, "Connected! IP: %s", s_ip);
            wifi_manager_start_ntp();
            return true;
        }
        ESP_LOGW(TAG, "Attempt %d failed", retry + 1);
    }

    ESP_LOGE(TAG, "Could not connect to '%s' after %d attempts", ssid, WIFI_CONNECT_RETRIES);
    return false;
}

bool wifi_manager_connect(uint32_t timeout_ms)
{
    if (!wifi_manager_begin_connect()) return false;
    return wifi_manager_wait_connect(timeout_ms);
}

// ---- NTP ------------------------------------------------------------

void wifi_manager_start_ntp(void)
{
    if (esp_sntp_enabled()) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_setservername(2, "cn.pool.ntp.org");
    esp_sntp_init();
    g_ntp_synced = false;
    ESP_LOGI(TAG, "NTP sync started");
}

// ---- Background monitor task -----------------------------------------

static void monitor_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        if (!s_connected && !s_ap_active) {
            ESP_LOGI(TAG, "Monitor: reconnecting...");
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
            esp_wifi_connect();
            EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                                   WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                   pdFALSE, pdFALSE,
                                                   pdMS_TO_TICKS(15000));
            if (bits & WIFI_CONNECTED_BIT) {
                wifi_manager_start_ntp();
                sonos_on_wifi_ready();
            }
        }

        // Check NTP sync status
        if (s_connected && !g_ntp_synced) {
            time_t now;
            time(&now);
            if (now > 1700000000) {  // sanity: after Nov 2023
                g_ntp_synced = true;
                ESP_LOGI(TAG, "NTP synced");
            }
        }
    }
}

void wifi_manager_start_monitor(void)
{
    xTaskCreate(monitor_task, "wifi_mon", 4096, NULL, 3, NULL);
}

// ---- Setup AP + captive portal ---------------------------------------

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html>"
    "<head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Music &amp; Meat Setup</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:420px;margin:32px auto;padding:0 20px;"
    "background:#111;color:#eee}"
    "h2{color:#1ED760;margin-bottom:4px}"
    "label{display:block;margin-top:14px;color:#aaa;font-size:.9em}"
    "input,select{width:100%%;padding:10px;margin-top:6px;background:#222;"
    "border:1px solid #444;border-radius:6px;color:#fff;font-size:1em;box-sizing:border-box}"
    "button{width:100%%;padding:13px;font-size:1em;border:0;border-radius:8px;"
    "cursor:pointer;margin-top:12px;font-weight:bold}"
    "#scan-btn{background:#333;color:#eee}"
    "#conn-btn{background:#1ED760;color:#000}"
    "#nets{display:none;margin-top:12px}"
    "#status{margin-top:14px;padding:12px;border-radius:6px;display:none}"
    ".ok{background:#1a4;color:#fff}.err{background:#811;color:#fff}"
    "</style></head><body>"
    "<h2>Music &amp; Meat</h2>"
    "<p style='color:#aaa;margin-top:0'>WiFi Setup</p>"
    "<button id='scan-btn' onclick='doScan()'>Scan for Networks</button>"
    "<div id='nets'>"
    "<label>Select Network</label>"
    "<select id='sel' onchange='document.getElementById(\"ssid\").value=this.value'>"
    "<option value=''>-- choose --</option>"
    "</select></div>"
    "<label>Network (SSID)</label>"
    "<input type='text' id='ssid' placeholder='Or type name here'>"
    "<label>Password</label>"
    "<input type='password' id='pass' placeholder='WiFi password'>"
    "<button id='conn-btn' onclick='doSave()'>Connect</button>"
    "<div id='status'></div>"
    "<script>"
    "function doScan(){"
    "var b=document.getElementById('scan-btn');"
    "b.textContent='Scanning…';b.disabled=true;"
    "fetch('/scan').then(r=>r.json()).then(nets=>{"
    "var s=document.getElementById('sel');"
    "s.innerHTML='<option value=\"\">-- choose --</option>';"
    "nets.forEach(n=>{"
    "var o=document.createElement('option');"
    "o.value=n.ssid;"
    "o.textContent=n.ssid+' '+n.bars;"
    "s.appendChild(o);});"
    "document.getElementById('nets').style.display='block';"
    "b.textContent='Scan Again';b.disabled=false;"
    "}).catch(()=>{b.textContent='Scan Again';b.disabled=false;});}"
    "function doSave(){"
    "var s=document.getElementById('ssid').value.trim();"
    "var p=document.getElementById('pass').value;"
    "if(!s){alert('Enter network name');return;}"
    "fetch('/save',{method:'POST',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)})"
    ".then(r=>r.text()).then(t=>{"
    "var el=document.getElementById('status');"
    "el.className='ok';el.style.display='block';el.textContent=t;"
    "}).catch(e=>{"
    "var el=document.getElementById('status');"
    "el.className='err';el.style.display='block';el.textContent='Error: '+e;"
    "});}"
    "</script></body></html>";

// ---- URL decode -------------------------------------------------------

static int hex_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void url_decode(const char *src, char *dst, size_t dst_sz)
{
    size_t i = 0;
    while (*src && i < dst_sz - 1) {
        if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            dst[i++] = (char)((hex_nib(src[1]) << 4) | hex_nib(src[2]));
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

// ---- HTTP handlers ---------------------------------------------------

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" WIFI_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t portal_scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t cfg = { .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    esp_wifi_scan_start(&cfg, true);   // blocking ~2 s

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 24) count = 24;

    wifi_ap_record_t *list = malloc(count * sizeof(wifi_ap_record_t));
    if (!list) {
        esp_wifi_scan_stop();
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&count, list);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < count; i++) {
        if (!list[i].ssid[0]) continue;
        char buf[128];
        // Basic JSON-safe SSID: escape backslash and double-quote
        char safe[70] = {0};
        int si = 0;
        for (int j = 0; list[i].ssid[j] && si < 66; j++) {
            char c = (char)list[i].ssid[j];
            if (c == '"' || c == '\\') safe[si++] = '\\';
            safe[si++] = c;
        }
        const char *bars = list[i].rssi > -50 ? "[****]" :
                           list[i].rssi > -60 ? "[*** ]" :
                           list[i].rssi > -70 ? "[**  ]" : "[*   ]";
        snprintf(buf, sizeof(buf), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"bars\":\"%s\"}",
                 i > 0 ? "," : "", safe, list[i].rssi, bars);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);  // end chunked response

    free(list);
    return ESP_OK;
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }

    char raw_ssid[128] = {0}, raw_pass[128] = {0};
    char *p = strstr(body, "ssid=");
    if (p) sscanf(p + 5, "%127[^&]", raw_ssid);
    p = strstr(body, "pass=");
    if (p) sscanf(p + 5, "%127[^&]", raw_pass);

    char ssid[64] = {0}, pass[64] = {0};
    url_decode(raw_ssid, ssid, sizeof(ssid));
    url_decode(raw_pass, pass, sizeof(pass));

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    wifi_manager_save_credentials(ssid, pass);
    httpd_resp_sendstr(req, "Saved! Connecting shortly — you can close this page.");

    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_stop_setup_ap();
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_connect();
    return ESP_OK;
}

// ---- DNS captive portal server ---------------------------------------
// Responds to all DNS A queries with the AP IP (192.168.4.1) so that
// any URL the phone visits redirects to our setup page.

static TaskHandle_t s_dns_task = NULL;

static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;

        // Build response: copy query, set QR+AA+RA flags, add 1 answer
        uint8_t resp[512];
        if (len + 16 > (int)sizeof(resp)) continue;
        memcpy(resp, buf, len);

        resp[2] = 0x81;  // QR=1, Opcode=0, AA=1
        resp[3] = 0x80;  // RA=1, no error
        resp[6] = 0x00;  resp[7] = 0x01;  // 1 answer RR
        resp[8] = 0x00;  resp[9] = 0x00;  // 0 authority RRs
        resp[10] = 0x00; resp[11] = 0x00; // 0 additional RRs

        // Answer: name pointer → offset 12 (start of question name)
        int pos = len;
        resp[pos++] = 0xC0; resp[pos++] = 0x0C;
        resp[pos++] = 0x00; resp[pos++] = 0x01;  // TYPE A
        resp[pos++] = 0x00; resp[pos++] = 0x01;  // CLASS IN
        resp[pos++] = 0x00; resp[pos++] = 0x00;
        resp[pos++] = 0x00; resp[pos++] = 0x3C;  // TTL 60 s
        resp[pos++] = 0x00; resp[pos++] = 0x04;  // RDLENGTH 4
        resp[pos++] = 192; resp[pos++] = 168;
        resp[pos++] = 4;   resp[pos++] = 1;

        sendto(sock, resp, pos, 0, (struct sockaddr *)&client, client_len);
    }

    close(sock);
    vTaskDelete(NULL);
}

// ---- AP start / stop ------------------------------------------------

void wifi_manager_start_setup_ap(void)
{
    if (s_ap_active) return;

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .channel        = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, WIFI_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(WIFI_AP_SSID);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // HTTP server — wildcard matching so the catch-all redirect works
    httpd_config_t srv_cfg = HTTPD_DEFAULT_CONFIG();
    srv_cfg.uri_match_fn     = httpd_uri_match_wildcard;
    srv_cfg.max_uri_handlers = 8;
    httpd_start(&s_portal_srv, &srv_cfg);

    // Register specific routes first — wildcard catch-all must be last
    static const httpd_uri_t routes[] = {
        { .uri = "/",     .method = HTTP_GET,  .handler = portal_get_handler      },
        { .uri = "/scan", .method = HTTP_GET,  .handler = portal_scan_handler     },
        { .uri = "/save", .method = HTTP_POST, .handler = portal_save_handler     },
        { .uri = "/*",    .method = HTTP_GET,  .handler = portal_redirect_handler },
    };
    for (int i = 0; i < 4; i++) httpd_register_uri_handler(s_portal_srv, &routes[i]);

    // DNS server — redirects all domains to 192.168.4.1
    xTaskCreate(dns_server_task, "dns_srv", 3072, NULL, 5, &s_dns_task);

    s_ap_active = true;
    ESP_LOGI(TAG, "Setup AP active — SSID: %s  http://%s", WIFI_AP_SSID, WIFI_AP_IP);
}

void wifi_manager_stop_setup_ap(void)
{
    if (!s_ap_active) return;
    if (s_portal_srv) {
        httpd_stop(s_portal_srv);
        s_portal_srv = NULL;
    }
    if (s_dns_task) {
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }
    s_ap_active = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "Setup AP stopped");
}

// ---- Accessors -------------------------------------------------------

bool        wifi_manager_is_connected(void) { return s_connected; }
bool        wifi_manager_ap_active(void)    { return s_ap_active; }
const char *wifi_manager_ssid(void)         { return s_connected ? s_ssid : ""; }
const char *wifi_manager_ip(void)           { return s_connected ? s_ip : ""; }

void wifi_manager_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_WIFI_SSID, ssid);
        nvs_set_str(nvs, NVS_KEY_WIFI_PASS, pass);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Saved credentials for '%s'", ssid);
    }
}
