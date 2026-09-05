#include "web_server.h"
#include "sonos_controller.h"
#include "wifi_manager.h"
#include "bbq_ble.h"
#include "bbq_controller.h"
#include "meat_temps.h"
#include "app_log.h"
#include "display.h"
#include "board_config.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_attr.h"     // EXT_RAM_BSS_ATTR
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

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
    "<title>Groove &amp; Grill</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#111;color:#eee;"
         "max-width:520px;margin:0 auto;padding:16px 12px}"
    "h1{color:#1db954;margin:0 0 4px;text-align:center}"
    /* Tab bar */
    ".tabbar{display:flex;gap:8px;margin:16px 0}"
    ".tabbar button{flex:1;background:#1a1a1a;color:#888;border:none;border-radius:8px;"
        "padding:10px;font-size:.85rem;font-weight:600;cursor:pointer}"
    ".tabbar button.active{background:#1e1e1e;color:#eee}"
    /* BBQ tab (scoped so it can't bleed into the Music tab's styling) */
    "#tab-bbq h2{color:#e87722;font-size:1.05em;text-align:center;margin-bottom:2px}"
    "#tab-bbq #s{color:#888;text-align:center;margin-bottom:16px;font-size:.85em}"
    "#tab-bbq .src-banner{background:#1a1a1a;border-radius:8px;padding:10px 14px;"
        "margin-bottom:14px;display:flex;align-items:center;justify-content:space-between;"
        "font-size:.85rem;gap:10px}"
    "#tab-bbq .src-banner b{color:#eee}"
    "#tab-bbq .card{background:#1c1c1c;border-radius:10px;padding:12px 14px;margin:10px 0}"
    "#tab-bbq .hdr{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:8px}"
    "#tab-bbq .name{font-weight:600}#tab-bbq .temp{font-size:1.3em;font-weight:700}"
    "#tab-bbq .ok{color:#1ed760}#tab-bbq .bad{color:#ff5555}"
    "#tab-bbq .ctl{display:grid;grid-template-columns:1fr 1fr;gap:10px;align-items:end}"
    "#tab-bbq .ctl label{font-size:.72rem;color:#999;display:block;margin-bottom:2px}"
    "#tab-bbq select,#tab-bbq input{background:#262626;border:1px solid #333;border-radius:6px;"
        "color:#eee;padding:6px 8px;font-size:.9rem}"
    "#tab-bbq .fld{min-width:0}#tab-bbq .fld select,#tab-bbq .fld input{width:100%;box-sizing:border-box}"
    "#tab-bbq button{background:#e87722;color:#000;border:none;border-radius:6px;"
        "padding:8px 14px;font-weight:600;margin-top:10px;cursor:pointer}"
    "#tab-bbq button:disabled{background:#444;color:#888}"
    "#tab-bbq .empty{color:#666;text-align:center;padding:24px 0}"
    "#tab-bbq .hide{display:none}"
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
    "<p class='footer'>Groove &amp; Grill &mdash; setup &middot; "
    "<a href='/faultlog' style='color:#555'>fault log</a></p></body></html>";

static const char *bbq_tab_content(void);   // defined below, used by setup_get_handler

// ---- GET /setup — combined Music Setup / BBQ Setup tabs -------------

static esp_err_t setup_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, HTML_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<h1>Groove &amp; Grill</h1>"
        "<div class='tabbar'>"
          "<button id='tabBtnMusic' class='active' onclick=\"showTab('music')\">Music Setup</button>"
          "<button id='tabBtnBbq' onclick=\"showTab('bbq')\">BBQ Setup</button>"
        "</div>"
        "<div id='tab-music'>"
        "<h2>Favourites &amp; Setup</h2>");

    // --- Device custom favourites ---
    // (The built-in Sonos favourites list is intentionally not shown here —
    // this page only manages the device's own custom favourites.)
    httpd_resp_sendstr_chunk(req, "<h3>Favourites</h3>");
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
        "<button id='btn-s' class='add-btn' type='button' onclick='submitStructured()'>+ Add</button>"
        "</div>"

        "<div class='or-sep'>or</div>"

        "<div class='add-card'>"
        "<label>Share URL</label>"
        "<input type='text' id='url' "
          "placeholder='https://open.spotify.com/playlist/37i9dQZF1EVJSvZp5AOML2'>"
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
          "if(!id){setBusy('btn-s',false);setStatus('');alert('Please enter an ID');return;}"
          "var params={source_type_id:src+'|'+typ+'|'+id,name:''};"
          "if(src==='spotify'){"
            "setStatus('Looking up Spotify info…');"
            "var spUrl='https://open.spotify.com/'+typ+'/'+id;"
            "resolveSpotify(spUrl,function(t,thumb){"
              "params.name=t;"
              "doAdd('/add_structured',params,thumb,'btn-s');"
            "});"
          "}else{"
            "doAdd('/add_structured',params,'','btn-s');"
          "}"
        "}"
        "function submitUrl(){"
          "setBusy('btn-u',true);setStatus('Working…');"
          "var url=document.getElementById('url').value.trim();"
          "if(!url){setBusy('btn-u',false);setStatus('');alert('Please enter a URL');return;}"
          "var params={url:url,name:''};"
          "if(url.indexOf('open.spotify.com')!==-1){"
            "setStatus('Looking up Spotify info…');"
            "resolveSpotify(url,function(t,thumb){"
              "params.name=t;"
              "doAdd('/add_by_url',params,thumb,'btn-u');"
            "});"
          "}else{"
            "doAdd('/add_by_url',params,'','btn-u');"
          "}"
        "}"
        "</script>"
        "</div>");   // close #tab-music

    httpd_resp_sendstr_chunk(req, "<div id='tab-bbq' style='display:none'>");
    httpd_resp_sendstr_chunk(req, bbq_tab_content());
    httpd_resp_sendstr_chunk(req, "</div>");   // close #tab-bbq

    httpd_resp_sendstr_chunk(req,
        "<script>"
        "function showTab(which){"
          "document.getElementById('tab-music').style.display=which=='music'?'':'none';"
          "document.getElementById('tab-bbq').style.display=which=='bbq'?'':'none';"
          "document.getElementById('tabBtnMusic').classList.toggle('active',which=='music');"
          "document.getElementById('tabBtnBbq').classList.toggle('active',which=='bbq');"
          "location.hash=which=='bbq'?'bbq':'';"
        "}"
        "if(location.hash=='#bbq')showTab('bbq');"
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
        LOGI(TAG, "Web play: %d (%s)", idx, sonos_favourite_name(idx));
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
        LOGI(TAG, "Add structured '%s' → %s: %s (idx=%d)", name, cmd, ok ? "ok" : "full", new_idx);
    } else {
        LOGW(TAG, "add_structured: bad fields src=%s typ=%s id=%s", source, type, id);
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
        LOGI(TAG, "Add URL '%s' → %s: %s (idx=%d)", name, cmd, ok ? "ok" : "full", new_idx);
    } else {
        LOGW(TAG, "add_by_url: unrecognised URL: %.80s", url);
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
        LOGI(TAG, "Del custom[%d]: %s", idx, ok ? "ok" : "bad index");
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
    LOGI(TAG, "upload_art idx=%d size=%d ok=%d", idx, received, ok);

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

// ---- BBQ sensor allocation page ------------------------------------
// One row per sensor (wired thermocouple or wireless probe) with its live
// temperature and dropdowns to allocate it: Grill # -> Type -> Meat -> Target.
// Writes go through bbq_sensor_assign() (NVS-persisted). The meat doneness
// options come from meat_temps.h so they match the on-screen selection.

// MEAT_TYPES order (Beef, Lamb, Pork) -> meat_kind_t enum value.
static const int s_meat_type_kind[MEAT_TYPE_COUNT] = {
    MEAT_KIND_BEEF, MEAT_KIND_LAMB, MEAT_KIND_PORK,
};

static esp_err_t bbq_data_handler(httpd_req_t *req)
{
    static char json[3072];   // single httpd worker -> static is safe
    int n = 0;

    n += snprintf(json + n, sizeof(json) - n,
                  "{\"grills\":%d,\"box\":%s,\"source\":%d,\"meats\":[",
                  MAX_GRILLS, bbq_ble_present() ? "true" : "false", (int)bbq_source_get());

    // Doneness tables (Beef/Lamb/Pork) + chicken's single food-safety target.
    for (int m = 0; m < MEAT_TYPE_COUNT; m++) {
        n += snprintf(json + n, sizeof(json) - n, "%s{\"kind\":%d,\"lv\":[",
                      m ? "," : "", s_meat_type_kind[m]);
        for (int l = 0; l < MEAT_TYPES[m].level_count; l++)
            n += snprintf(json + n, sizeof(json) - n, "%s[\"%s\",%d]",
                          l ? "," : "", MEAT_TYPES[m].levels[l].label,
                          MEAT_TYPES[m].levels[l].target_c);
        n += snprintf(json + n, sizeof(json) - n, "]}");
    }
    n += snprintf(json + n, sizeof(json) - n,
                  ",{\"kind\":%d,\"lv\":[[\"Safe\",%d]]}]",
                  MEAT_KIND_CHICKEN, CHICKEN_SAFE_TARGET_C);

    // Sensors.
    n += snprintf(json + n, sizeof(json) - n, ",\"sensors\":[");
    int count = bbq_sensor_count(), wnum = 0, emitted = 0;
    for (int i = 0; i < count; i++) {
        bbq_sensor_t s;
        if (!bbq_sensor_at(i, &s)) continue;
        char name[40];
        if (s.src == SRC_TC) snprintf(name, sizeof(name), "Wired Temp Sensor %d", s.hw_id + 1);
        else                 snprintf(name, sizeof(name), "Wireless Temp Sensor %d", ++wnum);
        n += snprintf(json + n, sizeof(json) - n,
                      "%s{\"src\":%d,\"hw\":%d,\"name\":\"%s\",\"present\":%s,\"temp\":",
                      emitted ? "," : "", (int)s.src, s.hw_id, name,
                      s.present ? "true" : "false");
        if (s.present) n += snprintf(json + n, sizeof(json) - n, "%.1f", s.temp_c);
        else           n += snprintf(json + n, sizeof(json) - n, "null");
        n += snprintf(json + n, sizeof(json) - n,
                      ",\"grill\":%d,\"role\":%d,\"kind\":%d,\"target\":%d}",
                      s.grill_num, (int)s.role, (int)s.meat_kind, s.target_c);
        emitted++;
    }
    snprintf(json + n, sizeof(json) - n, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// POST /bbq_assign  body: src=&hw=&grill=&role=&kind=&target=
static esp_err_t bbq_assign_handler(httpd_req_t *req)
{
    char body[128] = {0};
    int  len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body"); return ESP_FAIL; }
    body[len] = '\0';

    char v[16];
    form_field(body, "src",    v, sizeof(v)); int src    = atoi(v);
    form_field(body, "hw",     v, sizeof(v)); int hw     = atoi(v);
    form_field(body, "grill",  v, sizeof(v)); int grill  = atoi(v);
    form_field(body, "role",   v, sizeof(v)); int role   = atoi(v);
    form_field(body, "kind",   v, sizeof(v)); int kind   = atoi(v);
    form_field(body, "target", v, sizeof(v)); int target = atoi(v);

    bbq_sensor_assign((sensor_src_t)src, (uint8_t)hw, (uint8_t)grill,
                      (sensor_role_t)role, (meat_kind_t)kind, target);
    LOGI(TAG, "assign src=%d hw=%d -> grill=%d role=%d kind=%d target=%d",
             src, hw, grill, role, kind, target);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// ---- POST /bbq_source  body: mode=0(box)|1(probe) -------------------
// Switching BLE source needs a different NimBLE stack, so — same as the
// on-device Settings page — this persists the choice and reboots.

static void web_reboot_cb(void *arg) { (void)arg; esp_restart(); }

static esp_err_t bbq_source_handler(httpd_req_t *req)
{
    char body[32] = {0};
    int  len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body"); return ESP_FAIL; }
    body[len] = '\0';

    char v[8];
    form_field(body, "mode", v, sizeof(v));
    bbq_source_t mode = (atoi(v) == BBQ_SRC_PROBE) ? BBQ_SRC_PROBE : BBQ_SRC_BOX;
    bbq_source_set(mode);
    LOGI(TAG, "web: BBQ source -> %d, rebooting", (int)mode);

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, "{\"ok\":true}");   // flush before restarting

    const esp_timer_create_args_t targs = { .callback = web_reboot_cb, .name = "web_reboot" };
    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK)
        esp_timer_start_once(t, 800000);   // 800ms — let the HTTP response land first
    return r;
}

// ---- GET /bbq → redirect to the BBQ tab of the combined setup page --

static esp_err_t bbq_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/setup#bbq");
    return httpd_resp_send(req, NULL, 0);
}

// Body-only markup for the BBQ tab — embedded inside the combined /setup
// page (styles are the #tab-bbq-scoped rules in HTML_HEAD). Not sent as its
// own response; /bbq now just redirects here (see bbq_get_handler above).
static const char *bbq_tab_content(void)
{
    static const char CONTENT[] =
        "<h2>BBQ Sensors</h2>"
        "<div class='src-banner'>"
          "Source: <b id='srcLabel'>&hellip;</b>"
          "<button id='srcBtn' onclick='switchSource()' disabled>&hellip;</button>"
        "</div>"
        "<div id='s'>loading&hellip;</div><div id='list'></div>"
        "<script>"
        "var MEATS={},GRILLS=1,KEYS=[],CUR_SRC=0;"
        "var KINDS=[[4,'Beef'],[2,'Lamb'],[3,'Pork'],[1,'Chicken']];"
        "function opt(v,t,sel){return '<option value=\"'+v+'\"'+(sel?' selected':'')+'>'+t+'</option>';}"
        "function key(s){return s.src+'_'+s.hw;}"
        "function tgtOpts(kind,cur){var a=MEATS[kind]||[];var h='';"
          "for(var i=0;i<a.length;i++)h+=opt(a[i][1],a[i][0]+' ('+a[i][1]+'\\u00b0C)',a[i][1]==cur);return h;}"
        "function row(s){var k=key(s);"
          "var g='<label>Grill</label><select class=grill>'+opt(0,'\\u2014',!s.grill);"
          "for(var i=1;i<=GRILLS;i++)g+=opt(i,'Grill '+i,s.grill==i);g+='</select>';"
          "var r='<label>Type</label><select class=role>'+opt(0,'Unassigned',s.role==0)+"
            "opt(1,'Grill Temp',s.role==1)+opt(2,'Meat',s.role==2)+'</select>';"
          "var mk='<label>Meat</label><select class=kind>';"
          "for(var i=0;i<KINDS.length;i++)mk+=opt(KINDS[i][0],KINDS[i][1],s.kind==KINDS[i][0]);mk+='</select>';"
          "var tm='<label>Target</label><select class=tmeat>'+tgtOpts(s.kind||4,s.target)+'</select>';"
          "var tg='<label>Target \\u00b0C</label><input class=tgrill type=number step=5 min=40 max=400 value=\"'+(s.target||110)+'\">';"
          "return '<div class=card data-k=\"'+k+'\" data-src=\"'+s.src+'\" data-hw=\"'+s.hw+'\">'"
            "+'<div class=hdr><span class=name>'+s.name+'</span>"
                "<span class=\"temp '+(s.present?'ok':'bad')+'\" id=\"t_'+k+'\">'"
                "+(s.present?(s.temp.toFixed(1)+' \\u00b0C'):'Not connected')+'</span></div>'"
            "+'<div class=ctl><div class=fld>'+g+'</div><div class=fld>'+r+'</div>'"
                "+'<div class=\"fld mk\">'+mk+'</div><div class=\"fld tm\">'+tm+'</div>'"
                "+'<div class=\"fld tg\">'+tg+'</div></div>'"
            "+'<button class=save disabled>Saved</button></div>';}"
        "function sync(c){var role=+c.querySelector('.role').value;"
          "c.querySelector('.mk').classList.toggle('hide',role!=2);"
          "c.querySelector('.tm').classList.toggle('hide',role!=2);"
          "c.querySelector('.tg').classList.toggle('hide',role!=1);}"
        "function dirty(c){var b=c.querySelector('.save');b.disabled=false;b.textContent='Save';}"
        "function wire(c){"
          "c.querySelector('.role').onchange=function(){sync(c);dirty(c);};"
          "c.querySelector('.grill').onchange=function(){dirty(c);};"
          "c.querySelector('.kind').onchange=function(){"
            "var k=+this.value,cur=+c.querySelector('.tmeat').value;"
            "c.querySelector('.tmeat').innerHTML=tgtOpts(k,cur);dirty(c);};"
          "c.querySelector('.tmeat').onchange=function(){dirty(c);};"
          "c.querySelector('.tgrill').oninput=function(){dirty(c);};"
          "c.querySelector('.save').onclick=function(){save(c);};sync(c);}"
        "function save(c){var role=+c.querySelector('.role').value;"
          "var target=role==2?+c.querySelector('.tmeat').value:role==1?+c.querySelector('.tgrill').value:0;"
          "var body='src='+c.dataset.src+'&hw='+c.dataset.hw+'&grill='+c.querySelector('.grill').value"
            "+'&role='+role+'&kind='+c.querySelector('.kind').value+'&target='+target;"
          "var b=c.querySelector('.save');b.disabled=true;b.textContent='Saving\\u2026';"
          "fetch('/bbq_assign',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
            ".then(function(){b.textContent='Saved';}).catch(function(){b.textContent='Save';b.disabled=false;});}"
        "function build(d){GRILLS=d.grills;MEATS={};"
          "for(var i=0;i<d.meats.length;i++)MEATS[d.meats[i].kind]=d.meats[i].lv;"
          "var L=document.getElementById('list');"
          "if(!d.sensors.length){L.innerHTML='<div class=empty>No sensors detected yet.</div>';KEYS=[];return;}"
          "KEYS=d.sensors.map(key);L.innerHTML=d.sensors.map(row).join('');"
          "var cards=L.querySelectorAll('.card');for(var i=0;i<cards.length;i++)wire(cards[i]);}"
        "function refresh(d){for(var i=0;i<d.sensors.length;i++){var s=d.sensors[i];"
          "var t=document.getElementById('t_'+key(s));if(!t)continue;"
          "t.textContent=s.present?(s.temp.toFixed(1)+' \\u00b0C'):'Not connected';"
          "t.className='temp '+(s.present?'ok':'bad');}}"
        "function updateSrcBanner(d){CUR_SRC=d.source;"
          "document.getElementById('srcLabel').textContent="
            "d.source==1?'Wireless Probe (standalone)':'BBQ Box';"
          "var b=document.getElementById('srcBtn');"
          "b.disabled=false;b.textContent=d.source==1?'Switch to BBQ Box':'Switch to Wireless Probe';}"
        "function switchSource(){"
          "var next=CUR_SRC==1?0:1,label=next==1?'Wireless Probe':'BBQ Box';"
          "if(!confirm('Switch sensor source to '+label+'? The display will reboot.'))return;"
          "document.getElementById('srcBtn').disabled=true;"
          "fetch('/bbq_source',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
            "body:'mode='+next})"
            ".then(function(){document.getElementById('s').textContent='Rebooting\\u2026';})"
            ".catch(function(){});"
        "}"
        "function poll(first){fetch('/bbq_data').then(r=>r.json()).then(function(d){"
          "updateSrcBanner(d);"
          "document.getElementById('s').textContent=d.box?'box online':'box not heard \\u2014 scanning\\u2026';"
          "var kk=d.sensors.map(key).join(',');"
          "if(first||kk!=KEYS.join(','))build(d);else refresh(d);"
        "}).catch(function(){});}"
        "poll(true);setInterval(function(){poll(false);},2000);"
        "</script>";
    return CONTENT;
}

// ---- GET /faultlog — WARN/ERROR history, survives reboot/power loss ----
// Standalone page (own inline styles) rather than a tab in HTML_HEAD/
// setup_get_handler — this is a diagnostic tool, not part of the normal
// setup flow. Server-rendered, no JS/JSON round trip needed for something
// this small (fault volume is low by design — see app_log.c).

#define FAULTLOG_MAX_ROWS 200

// EXT_RAM_BSS_ATTR → PSRAM. This is ~24KB (200 * sizeof(app_log_fault_record_t))
// — as a plain `static` it was sitting permanently in internal DRAM for the
// whole life of the app regardless of whether this page is ever requested,
// on a chip where that budget is already razor-thin (WiFi + BLE + LVGL's
// pool leave headroom in the tens of KB). This one array accounted for the
// entire "Sonos poll_task/cmd_task fail to allocate stacks" regression.
static EXT_RAM_BSS_ATTR app_log_fault_record_t rows[FAULTLOG_MAX_ROWS];

static esp_err_t faultlog_get_handler(httpd_req_t *req)
{
    int n = app_log_read_faults(rows, FAULTLOG_MAX_ROWS);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta charset='utf-8'><title>Groove &amp; Grill &mdash; Fault Log</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;background:#111;color:#eee;"
             "max-width:640px;margin:0 auto;padding:16px 12px}"
        "h1{color:#1db954;margin:0 0 4px;text-align:center;font-size:1.1rem}"
        "p.sub{color:#888;text-align:center;margin:0 0 16px;font-size:.8rem}"
        ".row{background:#1c1c1c;border-radius:8px;padding:10px 12px;margin:6px 0;"
             "font-size:.85rem;border-left:3px solid #444}"
        ".row.warn{border-left-color:#e8b422}"
        ".row.error{border-left-color:#ff5555}"
        ".row .hdr{display:flex;justify-content:space-between;color:#999;"
                  "font-size:.72rem;margin-bottom:4px}"
        ".row .lvl{font-weight:700}"
        ".row.warn .lvl{color:#e8b422}.row.error .lvl{color:#ff5555}"
        ".row .msg{color:#eee;word-break:break-word}"
        ".empty{color:#666;text-align:center;padding:32px 0}"
        "button{background:#333;color:#e66;border:none;border-radius:6px;"
               "padding:10px 16px;font-size:.85rem;cursor:pointer;width:100%;margin-top:16px}"
        "</style></head><body>"
        "<h1>Fault Log</h1>");

    char sub[64];
    snprintf(sub, sizeof(sub), "<p class='sub'>%d entr%s (WARN/ERROR only)</p>",
             n, n == 1 ? "y" : "ies");
    httpd_resp_sendstr_chunk(req, sub);

    if (n == 0) {
        httpd_resp_sendstr_chunk(req, "<p class='empty'>No faults recorded.</p>");
    } else {
        char row[256];
        for (int i = n - 1; i >= 0; i--) {   // newest first
            const app_log_fault_record_t *r = &rows[i];
            const char *cls = (r->level == APP_LOG_ERROR) ? "error" : "warn";
            const char *lvl = (r->level == APP_LOG_ERROR) ? "ERROR" : "WARN";
            snprintf(row, sizeof(row),
                     "<div class='row %s'><div class='hdr'><span class='lvl'>%s</span>"
                     "<span>%s &middot; %lu ms</span></div><div class='msg'>%s</div></div>",
                     cls, lvl, r->tag, (unsigned long)r->timestamp_ms, r->msg);
            httpd_resp_sendstr_chunk(req, row);
        }
    }

    httpd_resp_sendstr_chunk(req,
        "<form method='POST' action='/faultlog_clear' "
        "onsubmit=\"return confirm('Clear the fault log?');\">"
        "<button type='submit'>Clear log</button></form>"
        "</body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t faultlog_clear_handler(httpd_req_t *req)
{
    app_log_clear_faults();
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/faultlog");
    return httpd_resp_send(req, NULL, 0);
}

// ---- GET /screenshot — dev-only: dumps the currently displayed frame as a
// BMP. LOCAL DEV TOOL, not meant to ship — reads the RGB panel's own PSRAM
// frame buffer directly (this display runs LVGL in direct_mode/avoid_tearing,
// so it's a real full-screen framebuffer, not something reconstructed via
// lv_snapshot_take()). Best used against a settled/static screen; mid-
// animation there's no guarantee which of the two swap buffers is "front".

static esp_err_t screenshot_get_handler(httpd_req_t *req)
{
    esp_lcd_panel_handle_t panel = display_get_panel();
    if (!panel) return httpd_resp_send_500(req);

    void *fb_void = NULL;
    if (esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb_void) != ESP_OK || !fb_void)
        return httpd_resp_send_500(req);
    const uint16_t *fb = (const uint16_t *)fb_void;   // RGB565, no byte-swap (LV_COLOR_16_SWAP off)

    if (!display_lock(pdMS_TO_TICKS(200)))
        return httpd_resp_send_500(req);

    const int      w = LCD_H_RES, h = LCD_V_RES;
    const uint32_t row_bytes        = (uint32_t)w * 3;   // 24bpp; 480*3=1440, already 4-byte aligned
    const uint32_t pixel_data_size  = row_bytes * (uint32_t)h;
    const uint32_t file_size        = 54 + pixel_data_size;

    // BMP: BITMAPFILEHEADER (14 bytes) + BITMAPINFOHEADER (40 bytes), 24bpp,
    // uncompressed, negative height = top-down row order (no need to reverse).
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(&hdr[2],  &file_size, 4);
    uint32_t data_offset = 54;
    memcpy(&hdr[10], &data_offset, 4);
    uint32_t dib_size = 40;
    memcpy(&hdr[14], &dib_size, 4);
    int32_t width = w, height = -h;
    memcpy(&hdr[18], &width, 4);
    memcpy(&hdr[22], &height, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(&hdr[26], &planes, 2);
    memcpy(&hdr[28], &bpp, 2);
    memcpy(&hdr[34], &pixel_data_size, 4);

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"screenshot.bmp\"");

    esp_err_t err = httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));

    uint8_t row[LCD_H_RES * 3];
    for (int y = 0; err == ESP_OK && y < h; y++) {
        const uint16_t *src = &fb[y * w];
        for (int x = 0; x < w; x++) {
            uint16_t px = src[x];
            uint8_t  r5 = (px >> 11) & 0x1F;
            uint8_t  g6 = (px >> 5)  & 0x3F;
            uint8_t  b5 =  px        & 0x1F;
            row[x * 3 + 0] = (uint8_t)((b5 << 3) | (b5 >> 2));   // B
            row[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));   // G
            row[x * 3 + 2] = (uint8_t)((r5 << 3) | (r5 >> 2));   // R
        }
        err = httpd_resp_send_chunk(req, (const char *)row, sizeof(row));
    }

    display_unlock();
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);
}

// ---- Public API -----------------------------------------------------

bool web_server_start(void)
{
    if (s_server) return true;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = 80;
    cfg.max_uri_handlers  = 14;
    cfg.recv_wait_timeout = 30;   // seconds — needed for 25KB art upload

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        LOGE(TAG, "Failed to start HTTP server");
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
        { .uri = "/bbq",           .method = HTTP_GET,  .handler = bbq_get_handler         },
        { .uri = "/bbq_data",      .method = HTTP_GET,  .handler = bbq_data_handler        },
        { .uri = "/bbq_assign",    .method = HTTP_POST, .handler = bbq_assign_handler      },
        { .uri = "/bbq_source",    .method = HTTP_POST, .handler = bbq_source_handler      },
        { .uri = "/faultlog",       .method = HTTP_GET,  .handler = faultlog_get_handler   },
        { .uri = "/faultlog_clear", .method = HTTP_POST, .handler = faultlog_clear_handler },
        { .uri = "/screenshot",     .method = HTTP_GET,  .handler = screenshot_get_handler },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++)
        httpd_register_uri_handler(s_server, &uris[i]);

    LOGI(TAG, "Setup server: http://%s/setup", wifi_manager_ip());
    return true;
}

void web_server_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
}

bool web_server_running(void)
{
    return s_server != NULL;
}
