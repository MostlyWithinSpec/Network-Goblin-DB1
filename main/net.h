#pragma once
#include <stdbool.h>
#include "esp_netif.h"

void net_init(void);
void net_poll(void);
bool net_has_creds(void);
void net_set_creds(const char *ssid, const char *pass);
void net_leave_setup(void);
void net_restart_ping(void);
esp_netif_t *net_sta_netif(void);

void scan_start_task(void);
void scan_request_now(void);
