#pragma once
#include <string.h>
static char q_item[128]; static int q_full;
static inline QueueHandle_t xQueueCreate(int n,int sz){return (void*)1;}
static inline int xQueueSend(QueueHandle_t q,const void*it,int t){memcpy(q_item,it,sizeof(q_item)<100?0:100);memcpy(q_item,it,68);q_full=1;return 1;}
static inline int xQueueSendToFront(QueueHandle_t q,const void*it,int t){return xQueueSend(q,it,t);}
static inline int xQueueReceive(QueueHandle_t q,void*it,int t){if(!q_full)return 0;memcpy(it,q_item,68);q_full=0;return 1;}
#define xTaskCreate(...) 0
#define vTaskDelay(x)
