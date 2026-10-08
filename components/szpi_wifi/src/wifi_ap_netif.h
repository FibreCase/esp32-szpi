#pragma once
#include "esp_netif.h"

/* Owner task only. Leaves DHCP explicitly stopped while setting AP address and
 * DNS; caller re-enables DHCP before radio startup. stage reports the failed
 * step and never contains credentials. */
esp_err_t szpi_wifi_ap_netif_prepare(esp_netif_t *netif, const char **stage);
