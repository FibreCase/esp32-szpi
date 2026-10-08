#pragma once
#include "esp_err.h"
#include "esp_netif_types.h"
typedef struct { int unused; } esp_netif_t;
typedef struct { struct { int type; union { test_ip_t ip4; } u_addr; } ip; } esp_netif_dns_info_t;
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 10
#define ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED 11
#define ESP_IPADDR_TYPE_V4 0
#define ESP_NETIF_DNS_MAIN 0
#define ESP_NETIF_OP_SET 0
#define ESP_NETIF_DOMAIN_NAME_SERVER 6
#define DHCPS_OFFER_DNS 2
#define ESP_IP4TOADDR(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))
esp_err_t esp_netif_dhcps_stop(esp_netif_t *netif);
esp_err_t esp_netif_set_ip_info(esp_netif_t *netif, const esp_netif_ip_info_t *ip);
esp_err_t esp_netif_set_dns_info(esp_netif_t *netif, int type, esp_netif_dns_info_t *dns);
esp_err_t esp_netif_dhcps_option(esp_netif_t *netif, int operation, int option, void *value, uint32_t size);
