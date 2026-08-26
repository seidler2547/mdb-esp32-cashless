#pragma once
/* Stub of the ESP-IDF header of the same name, just enough of it to build
 * main/mdb_debug.c on a host compiler. See ../README.md. */
#include <stdint.h>
#define ESP_OK 0
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v);
esp_err_t nvs_commit(nvs_handle_t h);
void nvs_close(nvs_handle_t h);
