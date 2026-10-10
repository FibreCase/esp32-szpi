#include "time_sync_service.h"

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "szpi_runtime_internal.h"

#define TAG "szpi_time_sync"

static bool s_sntp_initialized;
static char s_sntp_server[254];

static void time_sync_callback(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP synchronized; epoch=%lld", tv ? (long long)tv->tv_sec : 0LL);
    (void)tv;
    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_TIME_SYNCED);
}

esp_err_t szpi_time_sync_request(const esp_netif_ip_info_t *ip_info)
{
    char server[sizeof(s_sntp_server)];
#if CONFIG_SZPI_SNTP_SERVER_GATEWAY
    if (ip_info == NULL || ip_info->gw.addr == 0) {
        if (s_sntp_initialized) {
            esp_netif_sntp_deinit();
            s_sntp_initialized = false;
        }
        ESP_LOGW(TAG, "SNTP gateway unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(server, sizeof(server), IPSTR, IP2STR(&ip_info->gw));
#else
    (void)ip_info;
    const char *configured = CONFIG_SZPI_SNTP_SERVER_ADDRESS;
    if (configured[0] == '\0' || strlen(configured) >= sizeof(server)) {
        ESP_LOGW(TAG, "SNTP server address is empty or too long");
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(server, configured, strlen(configured) + 1);
#endif
    if (s_sntp_initialized && strcmp(server, s_sntp_server) != 0) {
        /* esp-netif marshals stop/init onto the TCP/IP thread. */
        esp_netif_sntp_deinit();
        s_sntp_initialized = false;
    }
    esp_err_t err;
    if (s_sntp_initialized) {
        err = esp_netif_sntp_start();
    } else {
        memcpy(s_sntp_server, server, strlen(server) + 1);
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_sntp_server);
        config.wait_for_sync = false;
        config.sync_cb = time_sync_callback;
        err = esp_netif_sntp_init(&config);
        if (err == ESP_OK) s_sntp_initialized = true;
    }
    if (err == ESP_OK) ESP_LOGI(TAG, "SNTP synchronization requested; server=%s", s_sntp_server);
    return err;
}
