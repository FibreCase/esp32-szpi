#pragma once
#include "szpi_wifi.h"
typedef enum {
    SZPI_WIFI_DISABLED, SZPI_WIFI_NO_CONFIG, SZPI_WIFI_STOPPED,
    SZPI_WIFI_CONNECTING, SZPI_WIFI_LINK_UP, SZPI_WIFI_ONLINE,
    SZPI_WIFI_RETRY_WAIT, SZPI_WIFI_FAILED, SZPI_WIFI_STOPPING,
} szpi_wifi_service_state_t;
typedef struct {
    szpi_wifi_service_state_t state;
    esp_netif_ip_info_t ip_info;
    int8_t rssi;
    bool rssi_valid;
    int last_disconnect_reason;
    uint32_t attempts;
    esp_err_t last_error;
    uint32_t event_queue_overflows;
    uint32_t sys_evt_stack_min_bytes;
    bool has_config;
    char ssid[33];
    szpi_provision_status_t provisioning;
} szpi_wifi_status_t;
