#pragma once
#include <stdint.h>
typedef struct { uint32_t addr; } test_ip_t;
typedef struct { test_ip_t ip, netmask, gw; } esp_netif_ip_info_t;
