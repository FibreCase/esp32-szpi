#ifndef SZPI_TIME_SYNC_SERVICE_H
#define SZPI_TIME_SYNC_SERVICE_H

#include "esp_err.h"
#include "esp_netif_types.h"

/* Wi-Fi-owner task only. Requests or restarts SNTP without waiting for sync.
 * Copies the selected server; ip_info is borrowed only for this call.
 * A changed gateway replaces the previous SNTP configuration. */
esp_err_t szpi_time_sync_request(const esp_netif_ip_info_t *ip_info);

#endif
