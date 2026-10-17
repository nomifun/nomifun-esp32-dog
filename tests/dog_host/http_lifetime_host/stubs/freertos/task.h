#pragma once
#include "FreeRTOS.h"
struct HostTask; using TaskHandle_t=HostTask*;
int xTaskCreate(void(*)(void*),const char*,int,void*,int,TaskHandle_t*);
TaskHandle_t xTaskGetCurrentTaskHandle();
void vTaskDelete(TaskHandle_t);
