// Network Goblin Desk Buddy - firmware for the ESP8684 (ESP32-C2) 1.54" weather cube
#include <stdio.h>
#include "state.h"
#include "lcd.h"
#include "ui.h"
#include "net.h"
#include "web.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "goblin";

void app_main(void)
{
    ESP_LOGI(TAG, "Network Goblin desk buddy v%s", FW_VERSION);
    state_init();
    ui_init();
    lcd_init();
    lcd_set_bl_invert(G.cfg.bl_invert);
    lcd_backlight(G.cfg.bright);
    ui_start();

    char hello[64];
    snprintf(hello, sizeof(hello), "Hi! I'm %s.|Lv %d %s", G.cfg.name, G.pet.level, pet_title(G.pet.level));
    ui_event(EV_HELLO, hello);

    net_init();
    web_start();
    scan_start_task();

    // new firmware booted fine -> keep it (cancels OTA rollback)
    esp_ota_mark_app_valid_cancel_rollback();

    int64_t connected_since = 0;
    bool announced = false;
    for (int sec = 0;; sec++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        net_poll();

        bool up, setup;
        char ip[16];
        LOCK(); up = G.wifi_up; setup = G.setup_mode; snprintf(ip, sizeof(ip), "%s", G.ip); UNLOCK();
        if (up && !announced) {
            char m[64];
            snprintf(m, sizeof(m), "Online! Visit me:|http://%s", ip);
            ui_event(EV_HELLO, m);
            announced = true;
        }
        if (!up) announced = false;

        // close the setup hotspot 3 minutes after a successful join
        if (up && setup) {
            if (!connected_since) connected_since = esp_timer_get_time();
            else if (esp_timer_get_time() - connected_since > 180LL * 1000000) net_leave_setup();
        } else {
            connected_since = 0;
        }

        if (sec % 60 == 59) pet_tick_minute();
    }
}
