#include "web.h"
#include "state.h"
#include "net.h"
#include "lcd.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lwip/inet.h"
#include "freertos/task.h"

static const char *TAG = "web";
extern const char index_html_start[] asm("_binary_index_html_gz_start");
extern const char index_html_end[]   asm("_binary_index_html_gz_end");
extern int s_render_ms;

// ------------------------------------------------------------------ helpers
static esp_err_t send_json(httpd_req_t *r, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(r, s ? s : "{}");
    free(s);
    return e;
}

static esp_err_t send_ok(httpd_req_t *r, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    if (msg) cJSON_AddStringToObject(j, "msg", msg);
    return send_json(r, j);
}

static cJSON *read_json(httpd_req_t *r)
{
    if (r->content_len <= 0 || r->content_len > 2048) return NULL;
    char *buf = malloc(r->content_len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, buf + got, r->content_len - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static void mac_str(const uint8_t *m, char *out)
{
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static bool parse_mac(const char *s, uint8_t *m)
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) m[i] = (uint8_t)v[i];
    return true;
}

static void reboot_later(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

// ------------------------------------------------------------------ handlers
static esp_err_t h_index(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    return httpd_resp_send(r, index_html_start, index_html_end - index_html_start);
}

static esp_err_t h_state(httpd_req_t *r)
{
    cJSON *j = cJSON_CreateObject();
    LOCK();
    cJSON_AddStringToObject(j, "fw", FW_VERSION);
    cJSON_AddStringToObject(j, "name", G.cfg.name);
    cJSON_AddBoolToObject(j, "wifi", G.wifi_up);
    cJSON_AddBoolToObject(j, "inet", G.inet_up);
    cJSON_AddBoolToObject(j, "setup", G.setup_mode);
    cJSON_AddStringToObject(j, "ip", G.ip);
    cJSON_AddStringToObject(j, "gw", G.gw);
    cJSON_AddStringToObject(j, "ssid", G.ssid);
    cJSON_AddNumberToObject(j, "rssi", G.rssi);
    cJSON_AddNumberToObject(j, "ping", G.ping_ms);
    cJSON_AddNumberToObject(j, "gw_ping", G.gw_ms);
    cJSON_AddNumberToObject(j, "loss", G.loss_pct);
    cJSON_AddNumberToObject(j, "slow", G.cfg.slow_ms_div10 * 10);
    cJSON *h = cJSON_AddArrayToObject(j, "hist");
    for (int i = 0; i < G.hist_count; i++) {
        int idx = (G.hist_head - G.hist_count + i + HIST_N) % HIST_N;
        cJSON_AddItemToArray(h, cJSON_CreateNumber(G.hist[idx]));
    }
    cJSON *o = cJSON_AddArrayToObject(j, "outages");
    int n = G.outage_n < OUTAGE_N ? G.outage_n : OUTAGE_N;
    for (int i = 0; i < n; i++) {
        outage_t *x = &G.outages[(G.outage_n - 1 - i) % OUTAGE_N];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "start", x->start);
        cJSON_AddNumberToObject(e, "dur", x->dur);
        cJSON_AddItemToArray(o, e);
    }
    cJSON_AddNumberToObject(j, "devices", G.dev_n);
    cJSON_AddNumberToObject(j, "online", G.dev_online);
    int unk = 0;
    for (int i = 0; i < G.dev_n; i++) if (!G.dev[i].known) unk++;
    cJSON_AddNumberToObject(j, "unknown", unk);
    cJSON_AddBoolToObject(j, "scanning", G.scanning);
    cJSON_AddNumberToObject(j, "last_scan", G.last_scan);
    cJSON *p = cJSON_AddObjectToObject(j, "pet");
    cJSON_AddNumberToObject(p, "level", G.pet.level);
    cJSON_AddStringToObject(p, "title", pet_title(G.pet.level));
    cJSON_AddNumberToObject(p, "xp", G.pet.xp);
    cJSON_AddNumberToObject(p, "xp_next", pet_xp_for(G.pet.level));
    cJSON_AddNumberToObject(p, "hunger", G.pet.hunger);
    cJSON_AddNumberToObject(p, "happy", G.pet.happy);
    cJSON_AddNumberToObject(p, "born", G.pet.born);
    cJSON_AddNumberToObject(p, "feeds", G.pet.feeds);
    cJSON_AddNumberToObject(p, "pats", G.pet.pats);
    cJSON_AddNumberToObject(p, "found", G.pet.found);
    cJSON_AddNumberToObject(p, "survived", G.pet.survived);
    cJSON_AddNumberToObject(j, "now", now_epoch());
    cJSON_AddBoolToObject(j, "have_time", G.have_time);
    cJSON_AddNumberToObject(j, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(j, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(j, "min_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(j, "render_ms", s_render_ms);
    UNLOCK();
    return send_json(r, j);
}

// Streamed one device at a time: building all 64 as one cJSON tree ran the C2 out of heap.
static esp_err_t h_devices(httpd_req_t *r)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(r, "[");
    char buf[256];
    for (int i = 0;; i++) {
        LOCK();
        if (i >= G.dev_n) { UNLOCK(); break; }
        device_t d = G.dev[i];
        UNLOCK();
        char m[18], name[50];
        mac_str(d.mac, m);
        // escape the user-supplied name for JSON
        int o = 0;
        for (const char *c = d.name; *c && o < (int)sizeof(name) - 3; c++) {
            if (*c == '"' || *c == '\\') name[o++] = '\\';
            if ((unsigned char)*c >= 0x20) name[o++] = *c;
        }
        name[o] = 0;
        ip4_addr_t ip = { .addr = d.ip };
        snprintf(buf, sizeof(buf),
                 "%s{\"mac\":\"%s\",\"ip\":\"%s\",\"name\":\"%s\",\"known\":%s,\"online\":%s,\"random\":%s,\"first\":%lu,\"last\":%lu}",
                 i ? "," : "", m, ip4addr_ntoa(&ip), name, d.known ? "true" : "false", d.online ? "true" : "false",
                 (d.mac[0] & 0x02) ? "true" : "false", (unsigned long)d.first_seen, (unsigned long)d.last_seen);
        if (httpd_resp_sendstr_chunk(r, buf) != ESP_OK) return ESP_FAIL;
    }
    httpd_resp_sendstr_chunk(r, "]");
    return httpd_resp_sendstr_chunk(r, NULL);
}

static esp_err_t h_device(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    if (!j) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json");
    cJSON *mac = cJSON_GetObjectItem(j, "mac");
    uint8_t m[6];
    if (!cJSON_IsString(mac) || !parse_mac(mac->valuestring, m)) { cJSON_Delete(j); return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "mac"); }
    LOCK();
    int idx = -1;
    for (int i = 0; i < G.dev_n; i++) if (!memcmp(G.dev[i].mac, m, 6)) { idx = i; break; }
    if (idx >= 0) {
        cJSON *v;
        if (cJSON_IsTrue(cJSON_GetObjectItem(j, "del"))) {
            G.dev[idx] = G.dev[G.dev_n - 1];
            G.dev_n--;
        } else {
            if (cJSON_IsString(v = cJSON_GetObjectItem(j, "name"))) strlcpy(G.dev[idx].name, v->valuestring, sizeof(G.dev[idx].name));
            if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "known"))) G.dev[idx].known = cJSON_IsTrue(v);
        }
    }
    UNLOCK();
    cJSON_Delete(j);
    devices_save();
    return send_ok(r, NULL);
}

static esp_err_t h_devices_all(httpd_req_t *r)
{
    // approve all / forget all
    cJSON *j = read_json(r);
    if (!j) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json");
    const char *act = cJSON_GetStringValue(cJSON_GetObjectItem(j, "action"));
    LOCK();
    if (act && !strcmp(act, "approve_all")) for (int i = 0; i < G.dev_n; i++) G.dev[i].known = 1;
    if (act && !strcmp(act, "forget_all")) { G.dev_n = 0; G.baseline_done = false; }
    UNLOCK();
    cJSON_Delete(j);
    devices_save();
    return send_ok(r, NULL);
}

static esp_err_t h_feed(httpd_req_t *r)
{
    int res = pet_feed();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", res == 0);
    cJSON_AddStringToObject(j, "msg", res == 0 ? "Nom nom nom!" : res == 1 ? "He's stuffed. Try later." : "Still chewing... wait a couple minutes.");
    return send_json(r, j);
}

static esp_err_t h_pat(httpd_req_t *r)
{
    const char *msg = "";
    pat_result_t res = pet_pat(&msg);
    static const char *moods[] = { "happy", "happy", "grumpy", "angry", "bit", "sulking" };
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", res <= PAT_HAPPY);
    cJSON_AddStringToObject(j, "msg", msg);
    cJSON_AddStringToObject(j, "mood", moods[res]);
    return send_json(r, j);
}
static esp_err_t h_scan(httpd_req_t *r) { scan_request_now(); return send_ok(r, "Scanning the LAN..."); }

static esp_err_t h_reboot(httpd_req_t *r)
{
    send_ok(r, "Rebooting");
    xTaskCreate(reboot_later, "rb", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_settings_get(httpd_req_t *r)
{
    cJSON *j = cJSON_CreateObject();
    LOCK();
    settings_t *c = &G.cfg;
    cJSON_AddStringToObject(j, "name", c->name);
    cJSON_AddStringToObject(j, "tz", c->tz);
    cJSON_AddStringToObject(j, "ping_host", c->ping_host);
    cJSON_AddNumberToObject(j, "sleep_start", c->sleep_start);
    cJSON_AddNumberToObject(j, "sleep_end", c->sleep_end);
    cJSON_AddNumberToObject(j, "bright", c->bright);
    cJSON_AddNumberToObject(j, "night_bright", c->night_bright);
    cJSON_AddBoolToObject(j, "bl_invert", c->bl_invert);
    cJSON_AddNumberToObject(j, "scan_min", c->scan_min);
    cJSON_AddNumberToObject(j, "slow_ms", c->slow_ms_div10 * 10);
    UNLOCK();
    return send_json(r, j);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static esp_err_t h_settings_set(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    if (!j) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json");
    cJSON *v;
    bool ping_changed = false, tz_changed = false;
    LOCK();
    settings_t *c = &G.cfg;
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "name")) && v->valuestring[0]) strlcpy(c->name, v->valuestring, sizeof(c->name));
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "tz")) && v->valuestring[0]) { strlcpy(c->tz, v->valuestring, sizeof(c->tz)); tz_changed = true; }
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "ping_host")) && v->valuestring[0] && strcmp(c->ping_host, v->valuestring)) {
        strlcpy(c->ping_host, v->valuestring, sizeof(c->ping_host));
        ping_changed = true;
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "sleep_start"))) c->sleep_start = clampi(v->valueint, 0, 23);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "sleep_end"))) c->sleep_end = clampi(v->valueint, 0, 23);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "bright"))) c->bright = clampi(v->valueint, 5, 100);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "night_bright"))) c->night_bright = clampi(v->valueint, 0, 100);
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "bl_invert"))) c->bl_invert = cJSON_IsTrue(v);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "scan_min"))) c->scan_min = clampi(v->valueint, 0, 240);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "slow_ms"))) c->slow_ms_div10 = clampi(v->valueint / 10, 3, 250);
    bool inv = c->bl_invert;
    char tz[48];
    strlcpy(tz, c->tz, sizeof(tz));
    UNLOCK();
    cJSON_Delete(j);
    settings_save();
    lcd_set_bl_invert(inv);
    if (tz_changed) { setenv("TZ", tz, 1); tzset(); }
    if (ping_changed) net_restart_ping();
    return send_ok(r, "Saved");
}

static esp_err_t h_wifiscan(httpd_req_t *r)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    cJSON *a = cJSON_CreateArray();
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 20;
        wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                if (!recs[i].ssid[0]) continue;
                bool dup = false;
                for (int k = 0; k < i; k++) if (!strcmp((char *)recs[k].ssid, (char *)recs[i].ssid)) dup = true;
                if (dup) continue;
                cJSON *e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "ssid", (char *)recs[i].ssid);
                cJSON_AddNumberToObject(e, "rssi", recs[i].rssi);
                cJSON_AddBoolToObject(e, "open", recs[i].authmode == WIFI_AUTH_OPEN);
                cJSON_AddItemToArray(a, e);
            }
        }
        free(recs);
    }
    return send_json(r, a);
}

static esp_err_t h_wifi(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    if (!j) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json");
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(j, "pass"));
    if (!ssid || !ssid[0]) { cJSON_Delete(j); return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "ssid"); }
    char s[33], p[65];
    strlcpy(s, ssid, sizeof(s));
    strlcpy(p, pass ? pass : "", sizeof(p));
    cJSON_Delete(j);
    net_set_creds(s, p);
    // wait up to 15 s to report the result back to the phone
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        bool up;
        LOCK(); up = G.wifi_up; UNLOCK();
        if (up) break;
    }
    cJSON *o = cJSON_CreateObject();
    LOCK();
    cJSON_AddBoolToObject(o, "ok", G.wifi_up);
    cJSON_AddStringToObject(o, "ip", G.ip);
    UNLOCK();
    return send_json(r, o);
}

static esp_err_t h_setup_done(httpd_req_t *r)
{
    send_ok(r, "Setup hotspot closed");
    net_leave_setup();
    return ESP_OK;
}

static esp_err_t h_update(httpd_req_t *r)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
    if (r->content_len <= 0 || r->content_len > part->size) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad size");
    esp_ota_handle_t ota;
    if (esp_ota_begin(part, r->content_len, &ota) != ESP_OK) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin");
    char *buf = malloc(2048);
    int left = r->content_len;
    bool fail = !buf;
    bool checked = false;
    while (!fail && left > 0) {
        int n = httpd_req_recv(r, buf, left < 2048 ? left : 2048);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { fail = true; break; }
        if (!checked) {
            checked = true;
            if ((uint8_t)buf[0] != 0xE9) { fail = true; break; }   // not an app image
        }
        if (esp_ota_write(ota, buf, n) != ESP_OK) { fail = true; break; }
        left -= n;
    }
    free(buf);
    if (fail) {
        esp_ota_abort(ota);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "upload failed or not a goblin .bin");
    }
    if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "image invalid");
    ESP_LOGI(TAG, "OTA ok -> %s", part->label);
    send_ok(r, "Updated! Rebooting...");
    xTaskCreate(reboot_later, "rb", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ------------------------------------------------------------------ server
void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 24;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    httpd_handle_t srv;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd failed"); return; }
    const httpd_uri_t uris[] = {
        { "/",                 HTTP_GET,  h_index, NULL },
        { "/api/state",        HTTP_GET,  h_state, NULL },
        { "/api/devices",      HTTP_GET,  h_devices, NULL },
        { "/api/device",       HTTP_POST, h_device, NULL },
        { "/api/devices",      HTTP_POST, h_devices_all, NULL },
        { "/api/feed",         HTTP_POST, h_feed, NULL },
        { "/api/pat",          HTTP_POST, h_pat, NULL },
        { "/api/scan",         HTTP_POST, h_scan, NULL },
        { "/api/reboot",       HTTP_POST, h_reboot, NULL },
        { "/api/settings",     HTTP_GET,  h_settings_get, NULL },
        { "/api/settings",     HTTP_POST, h_settings_set, NULL },
        { "/api/wifiscan",     HTTP_GET,  h_wifiscan, NULL },
        { "/api/wifi",         HTTP_POST, h_wifi, NULL },
        { "/api/setup_done",   HTTP_POST, h_setup_done, NULL },
        { "/api/update",       HTTP_POST, h_update, NULL },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(srv, &uris[i]);
    ESP_LOGI(TAG, "web up");
}
