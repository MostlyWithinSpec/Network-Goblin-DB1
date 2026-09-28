#include "state.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "state";
app_t G;
SemaphoreHandle_t G_lock;

#define CFG_VER 1
#define PET_VER 1

static void settings_default(settings_t *c)
{
    memset(c, 0, sizeof(*c));
    c->ver = CFG_VER;
    strcpy(c->tz, "EST5EDT,M3.2.0,M11.1.0");
    strcpy(c->ping_host, "1.1.1.1");
    c->sleep_start = 23;
    c->sleep_end = 7;
    c->bright = 70;
    c->night_bright = 6;
    c->bl_invert = 0;
    c->scan_min = 5;
    c->slow_ms_div10 = 15;   // 150 ms
    strcpy(c->name, "Gnorb");
}

static bool load_blob(const char *key, void *dst, size_t len)
{
    nvs_handle_t h;
    if (nvs_open("goblin", NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = len;
    esp_err_t e = nvs_get_blob(h, key, dst, &l);
    nvs_close(h);
    return e == ESP_OK && l == len;
}

static void save_blob(const char *key, const void *src, size_t len)
{
    nvs_handle_t h;
    if (nvs_open("goblin", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, src, len);
    nvs_commit(h);
    nvs_close(h);
}

void settings_save(void) { LOCK(); settings_t c = G.cfg; UNLOCK(); save_blob("cfg", &c, sizeof(c)); }
void pet_save(void)      { LOCK(); pet_t p = G.pet; UNLOCK(); save_blob("pet", &p, sizeof(p)); }

void devices_save(void)
{
    static device_t tmp[MAX_DEVICES];
    LOCK();
    int n = G.dev_n;
    memcpy(tmp, G.dev, sizeof(device_t) * n);
    UNLOCK();
    nvs_handle_t h;
    if (nvs_open("goblin", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "dev_n", (uint8_t)n);
    if (n) nvs_set_blob(h, "devs", tmp, sizeof(device_t) * n);
    nvs_commit(h);
    nvs_close(h);
}

static void devices_load(void)
{
    nvs_handle_t h;
    if (nvs_open("goblin", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t n = 0;
    if (nvs_get_u8(h, "dev_n", &n) == ESP_OK && n <= MAX_DEVICES) {
        size_t l = sizeof(device_t) * n;
        if (n == 0 || nvs_get_blob(h, "devs", G.dev, &l) == ESP_OK) {
            G.dev_n = n;
            G.baseline_done = n > 0;
        }
    }
    nvs_close(h);
    for (int i = 0; i < G.dev_n; i++) G.dev[i].online = 0;
}

uint32_t now_epoch(void)
{
    time_t t = time(NULL);
    if (t > 1700000000) return (uint32_t)t;
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

void state_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erase (%s)", esp_err_to_name(e));
        nvs_flash_erase();
        nvs_flash_init();
    }
    G_lock = xSemaphoreCreateRecursiveMutex();
    memset(&G, 0, sizeof(G));
    G.ping_ms = G.gw_ms = -1;
    if (!load_blob("cfg", &G.cfg, sizeof(G.cfg)) || G.cfg.ver != CFG_VER) settings_default(&G.cfg);
    if (!load_blob("pet", &G.pet, sizeof(G.pet)) || G.pet.ver != PET_VER) {
        memset(&G.pet, 0, sizeof(G.pet));
        G.pet.ver = PET_VER;
        G.pet.level = 1;
        G.pet.hunger = 80;
        G.pet.happy = 70;
    }
    devices_load();
    setenv("TZ", G.cfg.tz, 1);
    tzset();
}
