#pragma once
#include <stdint.h>
#include <stddef.h>
typedef void* SemaphoreHandle_t; typedef void* QueueHandle_t; typedef void* TaskHandle_t;
typedef int BaseType_t; typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffff
#define xSemaphoreTakeRecursive(a,b) 1
#define xSemaphoreGiveRecursive(a) 1
