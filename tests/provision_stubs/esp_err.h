#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_NOT_SUPPORTED 5
#define ESP_ERR_INVALID_RESPONSE 6
#define ESP_ERR_NVS_NOT_FOUND 7
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 8
#define ESP_ERR_WIFI_NOT_CONNECT 9
static inline const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock error"; }
