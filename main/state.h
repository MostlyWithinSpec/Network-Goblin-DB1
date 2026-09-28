#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define FW_VERSION "0.1.3"
#define HIST_N 60
#define OUTAGE_N 10
#define MAX_DEVICES 64

typedef struct {
    uint8_t ver;
    char tz[48];
    char ping_host[48];
    uint8_t sleep_start;   // hour 0-23
    uint8_t sleep_end;     // hour 0-23
    uint8_t bright;        // 0-100
    uint8_t night_bright;  // 0-100
    uint8_t bl_invert;
    uint8_t scan_min;      // minutes between LAN scans (0 = off)
    uint8_t slow_ms_div10; // "slow" threshold / 10 ms
    char name[20];         // goblin's name
} settings_t;

typedef struct {
    uint8_t mac[6];
    uint32_t ip;           // network order
    char name[24];
    uint8_t known;         // approved by the user
    uint8_t online;        // seen in the last scan
    uint32_t first_seen;   // epoch (or uptime seconds if no clock yet)
    uint32_t last_seen;
} device_t;

typedef struct {
    uint32_t start;
    uint32_t dur;          // seconds, 0 = ongoing
} outage_t;

typedef struct {
    uint8_t ver;
    uint32_t xp;
    uint16_t level;
    uint8_t hunger;        // 100 = stuffed, 0 = starving
    uint8_t happy;         // 0-100
    uint32_t born;
    uint32_t feeds, pats, found, survived;
    uint32_t last_fed;
} pet_t;

typedef enum {
    EV_NONE, EV_NEW_DEVICE, EV_FEED, EV_PAT, EV_LEVEL_UP, EV_OUTAGE, EV_RECOVER,
    EV_BASELINE, EV_TOO_FULL, EV_HELLO, EV_GRUMPY, EV_ANGRY, EV_BITE, EV_SULK,
} ev_type_t;

typedef struct {
    // network
    bool wifi_up, setup_mode, inet_up, have_time, scanning;
    char ip[16], gw[16], ssid[33], ap_ssid[33];
    int rssi;
    int ping_ms;           // last internet ping (-1 = lost)
    int gw_ms;             // last gateway ping (-1 = lost)
    int16_t hist[HIST_N];  // internet ping history (-1 = lost)
    int hist_head, hist_count;
    uint8_t loss_pct;      // over the history window
    outage_t outages[OUTAGE_N];
    int outage_n;          // entries used (ring, newest at [outage_n-1 % N])
    uint32_t outage_start; // 0 if internet is up
    // lan
    device_t dev[MAX_DEVICES];
    int dev_n;
    int dev_online;
    uint32_t last_scan;
    bool baseline_done;
    // pet
    pet_t pet;
    // settings
    settings_t cfg;
} app_t;

extern app_t G;
extern SemaphoreHandle_t G_lock;
#define LOCK()   xSemaphoreTakeRecursive(G_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(G_lock)

void state_init(void);
void settings_save(void);
void pet_save(void);
void devices_save(void);
uint32_t now_epoch(void);   // epoch if clock set, else uptime seconds

// UI events (shown one at a time as speech-bubble popups)
void ui_event(ev_type_t t, const char *text);
// shows immediately, replacing whatever popup is up (used for rapid pats)
void ui_event_now(ev_type_t t, const char *text);

// pet.c
void pet_tick_minute(void);
void pet_add_xp(int xp);
const char *pet_title(int level);
uint32_t pet_xp_for(int level);
int pet_feed(void);   // 0 ok, 1 too full, 2 cooldown
typedef enum { PAT_HAPPY_XP, PAT_HAPPY, PAT_GRUMPY, PAT_ANGRY, PAT_BIT, PAT_SULKING } pat_result_t;
pat_result_t pet_pat(const char **msg);
