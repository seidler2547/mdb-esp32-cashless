#pragma once
/* Stub of the ESP-IDF header of the same name, just enough of it to build
 * main/mdb_debug.c on a host compiler. See ../README.md. */
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
void vTaskDelay(uint32_t t);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, uint32_t prio, void *handle);
