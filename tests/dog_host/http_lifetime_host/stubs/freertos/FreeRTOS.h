#pragma once
#include <cstdint>
using TickType_t=uint32_t; using EventBits_t=uint32_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define BIT0 1
#define BIT1 2
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
