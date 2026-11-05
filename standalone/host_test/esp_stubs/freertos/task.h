#pragma once
#include "freertos/FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio, void *handle);
