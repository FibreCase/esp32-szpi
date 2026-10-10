#pragma once
#include <stdint.h>
typedef struct { uint32_t addr; } test_ip_t;
typedef struct { test_ip_t ip, netmask, gw; } esp_netif_ip_info_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) ((const uint8_t *)&(ip)->addr)[0], ((const uint8_t *)&(ip)->addr)[1], \
                  ((const uint8_t *)&(ip)->addr)[2], ((const uint8_t *)&(ip)->addr)[3]
