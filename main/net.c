#include "net.h"
#include "state.h"
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "freertos/event_groups.h"

static const char *TAG = "net";
static esp_netif_t *s_sta, *s_ap;
static esp_ping_handle_t s_ping_inet, s_ping_gw;
static int s_fail_streak;
static bool s_ever_connected;
static int64_t s_boot_us;
static bool s_sntp_started;

esp_netif_t *net_sta_netif(void) { return s_sta; }

// ------------------------------------------------------------------ ping
static void hist_push(int ms)
{
    G.hist[G.hist_head] = (int16_t)ms;
    G.hist_head = (G.hist_head + 1) % HIST_N;
    if (G.hist_count < HIST_N) G.hist_count++;
    int lost = 0;
    for (int i = 0; i < G.hist_count; i++) if (G.hist[i] < 0) lost++;
    G.loss_pct = (uint8_t)(lost * 100 / G.hist_count);
}

static void set_inet(bool up)
{
    char buf[64];
    LOCK();
    bool was = G.inet_up;
    G.inet_up = up;
    uint32_t now = now_epoch();
    if (was && !up) {
        G.outage_start = now;
        outage_t *o = &G.outages[G.outage_n % OUTAGE_N];
        o->start = now;
        o->dur = 0;
        G.outage_n++;
        UNLOCK();
        ui_event(EV_OUTAGE, "THE INTERNET|IS GONE!!");
        return;
    }
    if (!was && up) {
        uint32_t dur = 0;
        if (G.outage_start && G.outage_n) {
            dur = now - G.outage_start;
            G.outages[(G.outage_n - 1) % OUTAGE_N].dur = dur ? dur : 1;
            G.pet.survived++;
        }
        bool had = G.outage_start != 0;
        G.outage_start = 0;
        UNLOCK();
        if (had) {
            if (dur >= 60) snprintf(buf, sizeof(buf), "It's BACK!|Down %lum %lus", (unsigned long)(dur / 60), (unsigned long)(dur % 60));
            else snprintf(buf, sizeof(buf), "It's BACK!|Down %lus", (unsigned long)dur);
            ui_event(EV_RECOVER, buf);
            pet_add_xp(5);
        }
        return;
    }
    UNLOCK();
}

static void on_ping_ok(esp_ping_handle_t h, void *arg)
{
    uint32_t ms;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    if (arg) {  // internet
        LOCK();
        G.ping_ms = (int)ms;
        hist_push((int)ms);
        UNLOCK();
        s_fail_streak = 0;
        set_inet(true);
    } else {
        LOCK(); G.gw_ms = (int)ms; UNLOCK();
    }
}

static void on_ping_timeout(esp_ping_handle_t h, void *arg)
{
    if (arg) {
        LOCK();
        G.ping_ms = -1;
        hist_push(-1);
        UNLOCK();
        if (++s_fail_streak >= 3) set_inet(false);
    } else {
        LOCK(); G.gw_ms = -1; UNLOCK();
    }
}

static esp_ping_handle_t start_ping(ip_addr_t *target, bool inet)
{
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = *target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = inet ? 5000 : 10000;
    cfg.timeout_ms = 2000;
    cfg.task_stack_size = 3072;
    esp_ping_callbacks_t cbs = {
        .cb_args = inet ? (void *)1 : NULL,
        .on_ping_success = on_ping_ok,
        .on_ping_timeout = on_ping_timeout,
    };
    esp_ping_handle_t h = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &h) == ESP_OK) esp_ping_start(h);
    return h;
}

static void stop_pings(void)
{
    if (s_ping_inet) { esp_ping_stop(s_ping_inet); esp_ping_delete_session(s_ping_inet); s_ping_inet = NULL; }
    if (s_ping_gw)   { esp_ping_stop(s_ping_gw);   esp_ping_delete_session(s_ping_gw);   s_ping_gw = NULL; }
}

static void ping_task(void *arg)
{
    // resolve target (may be a hostname) and start both ping sessions
    char host[48];
    LOCK(); strlcpy(host, G.cfg.ping_host, sizeof(host)); UNLOCK();
    ip_addr_t target = {0};
    struct addrinfo hint = { .ai_family = AF_INET }, *res = NULL;
    for (int tries = 0; tries < 10; tries++) {
        if (getaddrinfo(host, NULL, &hint, &res) == 0 && res) break;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    if (res) {
        struct in_addr a = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        inet_addr_to_ip4addr(ip_2_ip4(&target), &a);
        target.type = IPADDR_TYPE_V4;
        freeaddrinfo(res);
        s_ping_inet = start_ping(&target, true);
    } else {
        ESP_LOGW(TAG, "can't resolve %s", host);
        set_inet(false);
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta, &ip) == ESP_OK && ip.gw.addr) {
        ip_addr_t gw = {0};
        ip_2_ip4(&gw)->addr = ip.gw.addr;
        gw.type = IPADDR_TYPE_V4;
        s_ping_gw = start_ping(&gw, false);
    }
    vTaskDelete(NULL);
}

void net_restart_ping(void)
{
    stop_pings();
    s_fail_streak = 0;
    xTaskCreate(ping_task, "pingstart", 3072, NULL, 4, NULL);
}

// ------------------------------------------------------------------ wifi
static void start_setup_ap(void)
{
    LOCK();
    if (G.setup_mode) { UNLOCK(); return; }
    G.setup_mode = true;
    UNLOCK();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap = { 0 };
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "Goblin-Setup-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = n;
    ap.ap.channel = 1;
    ap.ap.max_connection = 3;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    LOCK(); strlcpy(G.ap_ssid, (char *)ap.ap.ssid, sizeof(G.ap_ssid)); UNLOCK();
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    ESP_LOGI(TAG, "setup AP %s", ap.ap.ssid);
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool was;
        LOCK(); was = G.wifi_up; G.wifi_up = false; G.rssi = 0; UNLOCK();
        if (was) {
            stop_pings();
            set_inet(false);
        }
        // never connected after 45 s -> open the setup hotspot too
        if (!s_ever_connected && esp_timer_get_time() - s_boot_us > 45LL * 1000000) start_setup_ap();
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_ever_connected = true;
        LOCK();
        G.wifi_up = true;
        snprintf(G.ip, sizeof(G.ip), IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(G.gw, sizeof(G.gw), IPSTR, IP2STR(&e->ip_info.gw));
        bool setup = G.setup_mode;
        UNLOCK();
        ESP_LOGI(TAG, "got ip %s", G.ip);
        if (setup) {
            // keep the AP up for a bit so the phone can see the new IP, then drop it
            // (handled by the web layer after reporting success)
        }
        if (!s_sntp_started) {
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sc);
            s_sntp_started = true;
        }
        net_restart_ping();
    }
}

void net_leave_setup(void)
{
    LOCK();
    bool s = G.setup_mode;
    G.setup_mode = false;
    UNLOCK();
    if (s) esp_wifi_set_mode(WIFI_MODE_STA);
}

bool net_has_creds(void)
{
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) return false;
    return c.sta.ssid[0] != 0;
}

void net_set_creds(const char *ssid, const char *pass)
{
    wifi_config_t c = { 0 };
    strlcpy((char *)c.sta.ssid, ssid, sizeof(c.sta.ssid));
    strlcpy((char *)c.sta.password, pass, sizeof(c.sta.password));
    c.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    c.sta.pmf_cfg.capable = true;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &c);
    LOCK(); strlcpy(G.ssid, ssid, sizeof(G.ssid)); UNLOCK();
    esp_wifi_connect();
}

void net_init(void)
{
    s_boot_us = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, "network-goblin");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t c;
    bool creds = esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK && c.sta.ssid[0];
    if (creds) {
        LOCK(); strlcpy(G.ssid, (char *)c.sta.ssid, sizeof(G.ssid)); UNLOCK();
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    // USB powered: keep the radio awake so the web page and pings are snappy
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (!creds) start_setup_ap();
}

// periodic housekeeping: RSSI
void net_poll(void)
{
    wifi_ap_record_t ap;
    bool up;
    LOCK(); up = G.wifi_up; UNLOCK();
    if (up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        LOCK();
        G.rssi = ap.rssi;
        strlcpy(G.ssid, (char *)ap.ssid, sizeof(G.ssid));
        UNLOCK();
    }
    if (!G.have_time && time(NULL) > 1700000000) {
        LOCK(); G.have_time = true; UNLOCK();
        if (!G.pet.born) { LOCK(); G.pet.born = now_epoch(); UNLOCK(); pet_save(); }
    }
}
