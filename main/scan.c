// ARP sweep of the local /24 to find devices on the LAN the goblin is joined to.
#include "net.h"
#include "state.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "lwip/tcpip.h"
#include "lwip/etharp.h"
#include "lwip/inet.h"
#include "freertos/task.h"

static const char *TAG = "scan";
static SemaphoreHandle_t s_done;
static TaskHandle_t s_task;

#define BATCH 8

typedef struct {
    struct netif *nif;
    uint32_t base;       // host order, network address
    int from, to;        // host numbers
} arp_job_t;

typedef struct { uint8_t mac[6]; uint32_t ip; } hit_t;
static hit_t s_hits[MAX_DEVICES + 8];
static int s_hit_n;

static void cb_send(void *arg)
{
    arp_job_t *j = arg;
    for (int h = j->from; h <= j->to; h++) {
        ip4_addr_t a;
        a.addr = lwip_htonl(j->base + h);
        etharp_request(j->nif, &a);
    }
    xSemaphoreGive(s_done);
}

static void cb_read(void *arg)
{
    for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t *ip;
        struct netif *n;
        struct eth_addr *eth;
        if (etharp_get_entry(i, &ip, &n, &eth)) {
            bool dup = false;
            for (int k = 0; k < s_hit_n; k++) if (s_hits[k].ip == ip->addr) { dup = true; break; }
            if (!dup && s_hit_n < (int)(sizeof(s_hits) / sizeof(s_hits[0]))) {
                memcpy(s_hits[s_hit_n].mac, eth->addr, 6);
                s_hits[s_hit_n].ip = ip->addr;
                s_hit_n++;
            }
        }
    }
    xSemaphoreGive(s_done);
}

static void do_scan(void)
{
    esp_netif_t *sta = net_sta_netif();
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK || !info.ip.addr) return;
    struct netif *nif = esp_netif_get_netif_impl(sta);
    if (!nif) return;

    uint32_t ip = lwip_ntohl(info.ip.addr);
    uint32_t mask = lwip_ntohl(info.netmask.addr);
    if (mask < 0xFFFFFF00) mask = 0xFFFFFF00;   // cap the sweep at a /24
    uint32_t base = ip & mask;
    int hosts = (int)(~mask) - 1;

    LOCK(); G.scanning = true; UNLOCK();
    s_hit_n = 0;
    arp_job_t job = { .nif = nif, .base = base };
    for (int h = 1; h <= hosts; h += BATCH) {
        job.from = h;
        job.to = h + BATCH - 1 > hosts ? hosts : h + BATCH - 1;
        if (tcpip_callback(cb_send, &job) != ERR_OK) continue;
        xSemaphoreTake(s_done, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(350));
        if (tcpip_callback(cb_read, NULL) != ERR_OK) continue;
        xSemaphoreTake(s_done, portMAX_DELAY);
    }

    // merge hits into the device table
    uint32_t now = now_epoch();
    uint32_t self = info.ip.addr;
    char msg[64];
    int new_n = 0;
    char last_new[20] = "";
    LOCK();
    bool baseline = !G.baseline_done;
    for (int i = 0; i < G.dev_n; i++) G.dev[i].online = 0;
    for (int k = 0; k < s_hit_n; k++) {
        if (s_hits[k].ip == self) continue;
        int idx = -1;
        for (int i = 0; i < G.dev_n; i++)
            if (!memcmp(G.dev[i].mac, s_hits[k].mac, 6)) { idx = i; break; }
        if (idx < 0) {
            if (G.dev_n < MAX_DEVICES) idx = G.dev_n++;
            else {
                // evict the longest-unseen, non-approved device
                uint32_t oldest = UINT32_MAX;
                for (int i = 0; i < G.dev_n; i++)
                    if (!G.dev[i].known && G.dev[i].last_seen < oldest) { oldest = G.dev[i].last_seen; idx = i; }
                if (idx < 0) continue;
            }
            device_t *d = &G.dev[idx];
            memset(d, 0, sizeof(*d));
            memcpy(d->mac, s_hits[k].mac, 6);
            d->first_seen = now;
            d->known = baseline ? 1 : 0;
            new_n++;
            ip4_addr_t a = { .addr = s_hits[k].ip };
            snprintf(last_new, sizeof(last_new), "%s", ip4addr_ntoa(&a));
        }
        device_t *d = &G.dev[idx];
        d->ip = s_hits[k].ip;
        d->online = 1;
        d->last_seen = now;
    }
    int online = 0;
    for (int i = 0; i < G.dev_n; i++) online += G.dev[i].online;
    G.dev_online = online;
    G.last_scan = now;
    G.scanning = false;
    G.baseline_done = true;
    if (!baseline) G.pet.found += new_n;
    UNLOCK();

    ESP_LOGI(TAG, "scan: %d hits, %d new, %d online", s_hit_n, new_n, online);
    if (baseline) {
        snprintf(msg, sizeof(msg), "Met %d devices!|I'll guard them.", online);
        ui_event(EV_BASELINE, msg);
        pet_add_xp(10);
        devices_save();
    } else if (new_n) {
        if (new_n == 1) snprintf(msg, sizeof(msg), "NEW FACE!|%s", last_new);
        else snprintf(msg, sizeof(msg), "%d NEW FACES!|latest %s", new_n, last_new);
        ui_event(EV_NEW_DEVICE, msg);
        pet_add_xp(15 * new_n);
        devices_save();
    }
}

static void scan_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(15000));   // let DHCP/ping settle first
    for (;;) {
        bool up;
        int every;
        LOCK(); up = G.wifi_up; every = G.cfg.scan_min; UNLOCK();
        if (up && every) do_scan();
        int wait = every ? every * 60 : 300;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait * 1000));
    }
}

void scan_request_now(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void scan_start_task(void)
{
    s_done = xSemaphoreCreateBinary();
    xTaskCreate(scan_task, "scan", 4096, NULL, 3, &s_task);
}
