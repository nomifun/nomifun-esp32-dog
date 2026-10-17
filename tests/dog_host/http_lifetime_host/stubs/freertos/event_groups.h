#pragma once
#include "FreeRTOS.h"
struct HostEvent; using EventGroupHandle_t=HostEvent*;
EventGroupHandle_t xEventGroupCreate();
void vEventGroupDelete(EventGroupHandle_t);
EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupClearBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t,EventBits_t,int,int,TickType_t);
