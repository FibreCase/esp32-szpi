#pragma once
#include <stdbool.h>
#include <sys/time.h>
#include "esp_err.h"
typedef struct {
    bool wait_for_sync;
    void (*sync_cb)(struct timeval *);
    const char *servers[1];
} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) ((esp_sntp_config_t){.servers = {server}})
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
esp_err_t esp_netif_sntp_start(void);
void esp_netif_sntp_deinit(void);
