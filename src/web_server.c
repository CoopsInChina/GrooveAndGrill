#include "web_server.h"
#include "sonos_controller.h"
#include "wifi_manager.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web";
static httpd_handle_t s_server = NULL;

// ---- URL-form decode: decodes + as space and %XX in-place ----------

static void url_form_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' '; r++;
        } else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// ---- Parse a form field from a URL-encoded body --------------------
// Finds key= in body, writes up to out_sz-1 bytes to out, url-form-decodes it.

static void form_field(const char *body, const char *key, char *out, size_t out_sz)
{
    out[0] = '\0';
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) return;
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    url_form_decode(out);
}

// ---- Build API command from a Spotify or Apple Music share URL -----

static bool build_cmd_from_url(const char *raw_url, char *cmd_out, size_t cmd_sz,
                                char *name_out, size_t name_sz)
{
    // Spotify: https://open.spotify.com/{type}/{id}[?...]
    if (strstr(raw_url, "open.spotify.com")) {
        const char *after_host = strstr(raw_url, "open.spotify.com");
        const char *slash1 = strchr(after_host, '/');
        if (!slash1) return false;
        slash1++;  // skip first /

        // type segment
        const char *slash2 = strchr(slash1, '/');
        if (!slash2) return false;
        size_t type_len = (size_t)(slash2 - slash1);
        char type[16] = {0};
        if (type_len == 0 || type_len >= sizeof(type)) return false;
        memcpy(type, slash1, type_len);

        // id segment (strip ?... suffix)
        const char *id_start = slash2 + 1;
        const char *id_end = strpbrk(id_start, "?&");
        size_t id_len = id_end ? (size_t)(id_end - id_start) : strlen(id_start);
        char id[64] = {0};
        if (id_len == 0 || id_len >= sizeof(id)) return false;
        memcpy(id, id_start, id_len);

        snprintf(cmd_out, cmd_sz, "spotify/now/spotify:%s:%s", type, id);
        if (name_out && name_sz > 0 && name_out[0] == '\0') {
            // Auto-name: "Spotify Playlist" / "Spotify Album" etc.
            char cap_type[16];
            snprintf(cap_type, sizeof(cap_type), "%s", type);
            if (cap_type[0] >= 'a' && cap_type[0] <= 'z') cap_type[0] -= 32;
            snprintf(name_out, name_sz, "Spotify %s", cap_type);
        }
        return true;
    }

    // Apple Music: https://music.apple.com/{country}/{type}/{name}/{id}[?...]
    if (strstr(raw_url, "music.apple.com")) {
        // The last path segment before ? is the Apple Music ID
        const char *after_host = strstr(raw_url, "music.apple.com");
        const char *path_start = strchr(after_host, '/');
        if (!path_start) return false;

        const char *qmark = strchr(path_start, '?');
        const char *search_end = qmark ? qmark : (path_start + strlen(path_start));

        // Walk backward to find last '/'
        const char *last_slash = NULL;
        for (const char *c = search_end - 1; c >= path_start; c--) {
            if (*c == '/') { last_slash = c; break; }
        }
        if (!last_slash) return false;

        const char *id_start = last_slash + 1;
        size_t id_len = (size_t)(search_end - id_start);
        char id[64] = {0};
        if (id_len == 0 || id_len >= sizeof(id)) return false;
        memcpy(id, id_start, id_len);

        snprintf(cmd_out, cmd_sz, "applemusic/now/%s", id);
        if (name_out && name_sz > 0 && name_out[0] == '\0') {
            snprintf(name_out, name_sz, "Apple Music %s", id);
        }
        return true;
    }

    return false;
}

// ---- Build API command from structured fields ----------------------

static bool build_cmd_from_structured(const char *source, const char *type,
                                      const char *id, char *cmd_out, size_t cmd_sz,
                                      char *name_out, size_t name_sz)
{
    if (!id || !id[0]) return false;

    // Sanitise the id: drop any ?query (e.g. Spotify's ?si= share tag) and
    // trailing whitespace that can arrive when a full/pasted id is entered.
    char clean_id[96];
    snprintf(clean_id, sizeof(clean_id), "%s", id);
    char *qp = strpbrk(clean_id, "?&");
    if (qp) *qp = '\0';
    for (int i = (int)strlen(clean_id) - 1;
         i >= 0 && (clean_id[i] == '\n' || clean_id[i] == '\r' ||
                    clean_id[i] == ' '  || clean_id[i] == '\t'); i--)
        clean_id[i] = '\0';
    id = clean_id;
    if (!id[0]) return false;

    if (strcmp(source, "spotify") == 0) {
        const char *api_type = type;  // playlist / album / track
        snprintf(cmd_out, cmd_sz, "spotify/now/spotify:%s:%s", api_type, id);
        if (name_out && name_sz > 0 && name_out[0] == '\0') {
            char cap[16];
            snprintf(cap, sizeof(cap), "%s", type);
            if (cap[0] >= 'a' && cap[0] <= 'z') cap[0] -= 32;
            snprintf(name_out, name_sz, "Spotify %s", cap);
        }
        return true;
    }
    if (strcmp(source, "apple") == 0) {
        snprintf(cmd_out, cmd_sz, "applemusic/now/%s", id);
        if (name_out && name_sz > 0 && name_out[0] == '\0') {
            char cap[16];
            snprintf(cap, sizeof(cap), "%s", type);
            if (cap[0] >= 'a' && cap[0] <= 'z') cap[0] -= 32;
            snprintf(name_out, name_sz, "Apple %s", cap);
        }
        return true;
    }
    return false;
}

// ---- Shared HTML templates -----------------------------------------

static const char HTML_HEAD[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='utf-8'>"
    "<title>Music &amp; Meat</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#111;color:#eee;"
         "max-width:520px;margin:0 auto;padding:16px 12px}"
    "h1{color:#1db954;margin:0 0 4px}"
    "h2{color:#aaa;font-size:.85rem;font-weight:normal;margin:0 0 16px}"
    "h3{color:#888;font-size:.8rem;text-transform:uppercase;letter-spacing:.08em;"
         "margin:24px 0 8px;border-bottom:1px solid #333;padding-bottom:4px}"
    ".fav{display:flex;align-items:center;background:#1e1e1e;border-radius:10px;"
          "padding:12px 16px;margin:6px 0;gap:8px}"
    ".fav-name{font-size:1rem;flex:1;min-width:0;overflow:hidden;"
               "text-overflow:ellipsis;white-space:nowrap}"
    ".fav-cmd{font-size:.7rem;color:#555;margin-top:2px;"
              "overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".play-btn{background:#1db954;color:#000;border:none;border-radius:6px;"
               "padding:8px 14px;font-size:.9rem;cursor:pointer;flex-shrink:0}"
    ".play-btn:active{background:#17a348}"
    ".del-btn{background:#333;color:#e66;border:none;border-radius:6px;"
              "padding:8px 10px;font-size:.85rem;cursor:pointer;flex-shrink:0}"
    ".del-btn:active{background:#444}"
    ".empty{color:#666;text-align:center;padding:24px 0;font-size:.9rem}"

    /* Add form */
    ".add-card{background:#1a1a1a;border-radius:10px;padding:16px;margin-top:8px}"
    ".add-row{display:flex;gap:8px;margin-bottom:12px}"
    ".add-col{flex:1;min-width:0}"
    ".add-col.wide{flex:2}"
    ".add-card label{display:block;font-size:.78rem;color:#999;margin-bottom:4px}"
    ".add-card input[type=text],.add-card select{"
        "width:100%;box-sizing:border-box;background:#252525;"
        "border:1px solid #333;border-radius:6px;color:#eee;"
        "padding:9px 10px;font-size:.9rem;margin-bottom:12px}"
    ".add-card input[type=text]:focus,.add-card select:focus{outline:none;border-color:#1db954}"
    ".add-card select{appearance:none;-webkit-appearance:none;"
        "background-image:url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' "
        "width='12' height='8' viewBox='0 0 12 8'%3E%3Cpath d='M1 1l5 5 5-5' stroke='%23888' "
        "stroke-width='1.5' fill='none'/%3E%3C/svg%3E\");"
        "background-repeat:no-repeat;background-position:right 10px center;padding-right:28px}"
    ".or-sep{text-align:center;color:#555;margin:16px 0 8px;font-size:.85rem;"
             "display:flex;align-items:center;gap:8px}"
    ".or-sep::before,.or-sep::after{content:'';flex:1;height:1px;background:#333}"
    ".add-btn{background:#1db954;color:#000;border:none;border-radius:6px;"
              "padding:10px 20px;font-size:.9rem;cursor:pointer;width:100%}"
    ".add-btn:active{background:#17a348}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    ".add-btn.busy{opacity:.75;pointer-events:none;cursor:default}"
    ".add-btn.busy::after{content:'';display:inline-block;width:13px;height:13px;"
                         "border:2px solid rgba(0,0,0,.35);border-top-color:#000;"
                         "border-radius:50%;animation:spin .65s linear infinite;"
                         "vertical-align:middle;margin-left:8px}"
    ".footer{color:#555;font-size:.75rem;text-align:center;margin-top:28px}"
    ".status{display:none;background:#1a1a1a;border-radius:8px;padding:12px 16px;"
             "margin-top:12px;text-align:center;color:#1db954;font-size:.9rem}"
    ".status.err{color:#e66}"
    "</style></head><body>";

static const char HTML_TAIL[] =
    "<p class='footer'>Music &amp; Meat &mdash; setup</p></body></html>";

// ---- GET /setup -----------------------------------------------------

static esp_err_t setup_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, HTML_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>Music &amp; Meat</h1><h2>Favourites &amp; Setup</h2>");

    // --- Sonos built-in favourites ---
    httpd_resp_sendstr_chunk(req, "<h3>Sonos Favourites</h3>");
    int sonos_count = sonos_favourites_count() - sonos_device_fav_count();
    if (sonos_count <= 0) {
        httpd_resp_sendstr_chunk(req,
            "<p class='empty'>No Sonos favourites loaded yet.<br>"
            "Ensure the API server is running.</p>");
    } else {
        char buf[320];
        for (int i = 0; i < sonos_count; i++) {
            snprintf(buf, sizeof(buf),
                "<div class='fav'>"
                "<span class='fav-name'>%s</span>"
                "<form method='POST' action='/play' style='margin:0'>"
                "<input type='hidden' name='idx' value='%d'>"
                "<button class='play-btn' type='submit'>&#9654;</button>"
                "</form></div>",
                sonos_favourite_name(i), i);
            httpd_resp_sendstr_chunk(req, buf);
        }
    }

    // --- Device custom favourites ---
    httpd_resp_sendstr_chunk(req, "<h3>Custom Favourites</h3>");
    int dev_count = sonos_device_fav_count();
    int base_idx  = sonos_favourites_count() - dev_count;
    if (dev_count == 0) {
        httpd_resp_sendstr_chunk(req,
            "<p class='empty'>No custom favourites yet. Add one below.</p>");
    } else {
        char buf[800];
        for (int i = 0; i < dev_count; i++) {
            snprintf(buf, sizeof(buf),
                "<div class='fav'>"
                "<div style='flex:1;min-width:0'>"
                "<div class='fav-name'>%s</div>"
                "<div class='fav-cmd'>%s</div>"
                "</div>"
                "<form method='POST' action='/play' style='margin:0'>"
                "<input type='hidden' name='idx' value='%d'>"
                "<button class='play-btn' type='submit'>&#9654;</button>"
                "</form>"
                "<form method='POST' action='/del_custom' style='margin:0'>"
                "<input type='hidden' name='idx' value='%d'>"
                "<button class='del-btn' type='submit'>&#x2715;</button>"
                "</form></div>",
                sonos_device_fav_name(i), sonos_device_fav_cmd(i),
                base_idx + i, i);
            httpd_resp_sendstr_chunk(req, buf);
        }
    }

    // --- Add favourite: structured (Source + Type + ID) ---
    httpd_resp_sendstr_chunk(req, "<h3>Add Favourite</h3>");
    httpd_resp_sendstr_chunk(req,
        "<div class='add-card'>"
        "<div class='add-row'>"
          "<div class='add-col'>"
            "<label>Source</label>"
            "<select name='source' id='src' onchange='srcChanged()'>"
              "<option value='spotify'>Spotify</option>"
              "<option value='apple'>Apple Music</option>"
            "</select>"
          "</div>"
          "<div class='add-col'>"
            "<label>Type</label>"
            "<select name='type' id='typ'>"
              "<option value='playlist'>Playlist</option>"
              "<option value='album'>Album</option>"
              "<option value='track'>Track</option>"
            "</select>"
          "</div>"
          "<div class='add-col wide'>"
            "<label>ID</label>"
            "<input type='text' id='sid' placeholder='37i9dQZF1EVJSvZp5AOML2'>"
          "</div>"
        "</div>"
        "<label>Name (shown on device)</label>"
        "<input type='text' id='sname' placeholder='Leave blank to auto-generate'>"
        "<label>Art image URL <span style='color:#555'>(optional — Spotify auto-fetches)</span></label>"
        "<input type='text' id='sart' placeholder='https://...'>"
        "<button id='btn-s' class='add-btn' type='button' onclick='submitStructured()'>+ Add</button>"
        "</div>"

        "<div class='or-sep'>or</div>"

        "<div class='add-card'>"
        "<label>Share URL</label>"
        "<input type='text' id='url' "
          "placeholder='https://open.spotify.com/playlist/37i9dQZF1EVJSvZp5AOML2'>"
        "<label>Name (shown on device)</label>"
        "<input type='text' id='uname' placeholder='Leave blank to auto-generate'>"
        "<label>Art image URL <span style='color:#555'>(optional — Spotify auto-fetches)</span></label>"
        "<input type='text' id='uart' placeholder='https://...'>"
        "<button id='btn-u' class='add-btn' type='button' onclick='submitUrl()'>+ Add from URL</button>"
        "</div>"

        "<div id='status' class='status'></div>"

        "<script>"
        "function setStatus(msg,err){"
          "var d=document.getElementById('status');"
          "d.textContent=msg;"
          "d.className=err?'status err':'status';"
          "d.style.display=msg?'block':'none';"
        "}"
        "function setBusy(id,busy){"
          "var b=document.getElementById(id);"
          "if(!b)return;"
          "b.disabled=busy;"
          "b.classList[busy?'add':'remove']('busy');"
        "}"
        "var spTypes=['playlist','album','track'];"
        "var apTypes=['playlist','album','song'];"
        "function srcChanged(){"
          "var src=document.getElementById('src').value;"
          "var t=document.getElementById('typ');"
          "var types=src==='spotify'?spTypes:apTypes;"
          "t.innerHTML=types.map(function(v){"
            "return '<option value=\"'+v+'\">'+v.charAt(0).toUpperCase()+v.slice(1)+'</option>';"
          "}).join('');"
        "}"
        // Resolve Spotify oEmbed with 5s timeout (GFW-safe)
        "function resolveSpotify(spUrl,cb){"
          "var done=false;"
          "var timer=setTimeout(function(){if(!done){done=true;cb('','');}},5000);"
          "fetch('https://open.spotify.com/oembed?url='+encodeURIComponent(spUrl))"
            ".then(function(r){return r.json();})"
            ".then(function(d){if(!done){done=true;clearTimeout(timer);cb(d.title||'',d.thumbnail_url||'');}})"
            ".catch(function(){if(!done){done=true;clearTimeout(timer);cb('','');}});"
        "}"
        // Upload JPEG from artUrl to /upload_art?idx=N
        "function uploadArt(idx,artUrl){"
          "if(!artUrl)return Promise.resolve();"
          "setStatus('Downloading album art…');"
          "return fetch(artUrl)"
            ".then(function(r){if(!r.ok)throw new Error('art '+r.status);return r.arrayBuffer();})"
            ".then(function(buf){"
              "setStatus('Saving art to device ('+Math.round(buf.byteLength/1024)+'KB)…');"
              "return fetch('/upload_art?idx='+idx,{"
                "method:'POST',"
                "headers:{'Content-Type':'application/octet-stream'},"
                "body:buf"
              "});"
            "})"
            ".catch(function(e){console.warn('art upload:',e);setStatus('Art upload failed — favourite saved without art',true);});"
        "}"
        // POST to endpoint, then upload art, then reload
        "function doAdd(endpoint,params,artUrl,btnId){"
          "setStatus('Adding favourite…');"
          "fetch(endpoint,{"
            "method:'POST',"
            "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
            "body:new URLSearchParams(params)"
          "})"
          ".then(function(r){return r.json();})"
          ".then(function(d){"
            "if(!d.ok){setBusy(btnId,false);setStatus('Failed — list may be full',true);return Promise.resolve();}"
            "if(artUrl)return uploadArt(d.idx,artUrl);"
            "return Promise.resolve();"
          "})"
          ".then(function(){setStatus('Done! Reloading…');window.location='/setup';})"
          ".catch(function(e){console.warn(e);setBusy(btnId,false);window.location='/setup';});"
        "}"
        "function submitStructured(){"
          "setBusy('btn-s',true);setStatus('Working…');"
          "var src=document.getElementById('src').value;"
          "var typ=document.getElementById('typ').value;"
          "var id=document.getElementById('sid').value.trim();"
          "var nm=document.getElementById('sname').value.trim();"
          "var art=document.getElementById('sart').value.trim();"
          "if(!id){setBusy('btn-s',false);setStatus('');alert('Please enter an ID');return;}"
          "var params={source_type_id:src+'|'+typ+'|'+id,name:nm};"
          "if(!nm&&src==='spotify'){"
            "setStatus('Looking up Spotify info…');"
            "var spUrl='https://open.spotify.com/'+typ+'/'+id;"
            "resolveSpotify(spUrl,function(t,thumb){"
              "params.name=t;"
              "doAdd('/add_structured',params,art||thumb,'btn-s');"
            "});"
          "}else{"
            "doAdd('/add_structured',params,art,'btn-s');"
          "}"
        "}"
        "function submitUrl(){"
          "setBusy('btn-u',true);setStatus('Working…');"
          "var url=document.getElementById('url').value.trim();"
          "var nm=document.getElementById('uname').value.trim();"
          "var art=document.getElementById('uart').value.trim();"
          "if(!url){setBusy('btn-u',false);setStatus('');alert('Please enter a URL');return;}"
          "var params={url:url,name:nm};"
          "if(!nm&&url.indexOf('open.spotify.com')!==-1){"
            "setStatus('Looking up Spotify info…');"
            "resolveSpotify(url,function(t,thumb){"
              "params.name=t;"
              "doAdd('/add_by_url',params,art||thumb,'btn-u');"
            "});"
          "}else{"
            "doAdd('/add_by_url',params,art,'btn-u');"
          "}"
        "}"
        "</script>");

    httpd_resp_sendstr_chunk(req, HTML_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ---- POST /play -----------------------------------------------------

static esp_err_t play_post_handler(httpd_req_t *req)
{
    char body[64] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body"); return ESP_FAIL; }
    body[n] = '\0';
    int idx = -1;
    char *p = strstr(body, "idx=");
    if (p) idx = atoi(p + 4);
    if (idx >= 0 && idx < sonos_favourites_count()) {
        sonos_play_favourite((uint8_t)idx);
        ESP_LOGI(TAG, "Web play: %d (%s)", idx, sonos_favourite_name(idx));
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/setup");
    return httpd_resp_send(req, NULL, 0);
}

// ---- POST /add_structured — Source + Type + ID ---------------------

static esp_err_t add_structured_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body"); return ESP_FAIL; }
    body[n] = '\0';

    char name[64] = {0};
    char sti[128] = {0};  // "source|type|id"
    form_field(body, "name",          name, sizeof(name));
    form_field(body, "source_type_id", sti,  sizeof(sti));

    // Parse source|type|id
    char source[16] = {0}, type[16] = {0}, id[64] = {0};
    char *p1 = strchr(sti, '|');
    if (p1) {
        size_t slen = (size_t)(p1 - sti);
        if (slen < sizeof(source)) { memcpy(source, sti, slen); }
        char *p2 = strchr(p1 + 1, '|');
        if (p2) {
            size_t tlen = (size_t)(p2 - (p1 + 1));
            if (tlen < sizeof(type)) { memcpy(type, p1 + 1, tlen); }
            strlcpy(id, p2 + 1, sizeof(id));
        }
    }

    char cmd[256] = {0};
    bool ok = false;
    int  new_idx = -1;
    if (source[0] && type[0] && id[0] &&
        build_cmd_from_structured(source, type, id, cmd, sizeof(cmd), name, sizeof(name))) {
        ok = sonos_add_device_favourite(name, cmd);
        if (ok) new_idx = sonos_device_fav_count() - 1;
        ESP_LOGI(TAG, "Add structured '%s' → %s: %s (idx=%d)", name, cmd, ok ? "ok" : "full", new_idx);
    } else {
        ESP_LOGW(TAG, "add_structured: bad fields src=%s typ=%s id=%s", source, type, id);
    }
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"idx\":%d}", ok ? "true" : "false", new_idx);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

// ---- POST /add_by_url — parse share URL ----------------------------

static esp_err_t add_by_url_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body"); return ESP_FAIL; }
    body[n] = '\0';

    char name[64] = {0};
    char url[512] = {0};
    form_field(body, "name", name, sizeof(name));
    form_field(body, "url",  url,  sizeof(url));

    char cmd[256] = {0};
    bool ok = false;
    int  new_idx = -1;
    if (url[0] && build_cmd_from_url(url, cmd, sizeof(cmd), name, sizeof(name))) {
        ok = sonos_add_device_favourite(name, cmd);
        if (ok) new_idx = sonos_device_fav_count() - 1;
        ESP_LOGI(TAG, "Add URL '%s' → %s: %s (idx=%d)", name, cmd, ok ? "ok" : "full", new_idx);
    } else {
        ESP_LOGW(TAG, "add_by_url: unrecognised URL: %.80s", url);
    }
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"idx\":%d}", ok ? "true" : "false", new_idx);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

// ---- POST /del_custom ----------------------------------------------

static esp_err_t del_custom_handler(httpd_req_t *req)
{
    char body[32] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body"); return ESP_FAIL; }
    body[n] = '\0';
    int idx = -1;
    char *p = strstr(body, "idx=");
    if (p) idx = atoi(p + 4);
    if (idx >= 0) {
        bool ok = sonos_remove_device_favourite(idx);
        ESP_LOGI(TAG, "Del custom[%d]: %s", idx, ok ? "ok" : "bad index");
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/setup");
    return httpd_resp_send(req, NULL, 0);
}

// ---- POST /upload_art?idx=N — receive JPEG bytes, store in SPIFFS --------

static esp_err_t upload_art_handler(httpd_req_t *req)
{
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char idx_str[8] = {0};
    httpd_query_key_value(query, "idx", idx_str, sizeof(idx_str));
    int idx = atoi(idx_str);

    int content_len = req->content_len;
    if (content_len <= 0 || content_len > (int)(64 * 1024)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size out of range");
        return ESP_FAIL;
    }

    uint8_t *buf = heap_caps_malloc((size_t)content_len,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, (char *)buf + received,
                                 content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (ret <= 0) {
            heap_caps_free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv err");
            return ESP_FAIL;
        }
        received += ret;
    }

    bool ok = sonos_set_device_fav_art(idx, buf, (size_t)received);
    heap_caps_free(buf);
    ESP_LOGI(TAG, "upload_art idx=%d size=%d ok=%d", idx, received, ok);

    char resp[32];
    snprintf(resp, sizeof(resp), "{\"ok\":%s}", ok ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

// ---- GET / → redirect ----------------------------------------------

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/setup");
    return httpd_resp_send(req, NULL, 0);
}

// ---- Public API -----------------------------------------------------

bool web_server_start(void)
{
    if (s_server) return true;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = 80;
    cfg.max_uri_handlers  = 10;
    cfg.recv_wait_timeout = 30;   // seconds — needed for 25KB art upload

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return false;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",              .method = HTTP_GET,  .handler = root_handler           },
        { .uri = "/setup",         .method = HTTP_GET,  .handler = setup_get_handler       },
        { .uri = "/play",          .method = HTTP_POST, .handler = play_post_handler       },
        { .uri = "/add_structured",.method = HTTP_POST, .handler = add_structured_handler  },
        { .uri = "/add_by_url",    .method = HTTP_POST, .handler = add_by_url_handler      },
        { .uri = "/del_custom",    .method = HTTP_POST, .handler = del_custom_handler      },
        { .uri = "/upload_art",    .method = HTTP_POST, .handler = upload_art_handler      },
    };
    for (int i = 0; i < 7; i++)
        httpd_register_uri_handler(s_server, &uris[i]);

    ESP_LOGI(TAG, "Setup server: http://%s/setup", wifi_manager_ip());
    return true;
}

bool web_server_running(void)
{
    return s_server != NULL;
}
