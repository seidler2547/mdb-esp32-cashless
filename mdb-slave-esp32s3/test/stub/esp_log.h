#pragma once
/* Stub of the ESP-IDF header of the same name, just enough of it to build
 * main/mdb_debug.c on a host compiler. See ../README.md. */
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("[I %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E %s] " fmt "\n", tag, ##__VA_ARGS__)
