#include "wifi_ap_netif.h"
#include "dhcpserver/dhcpserver.h"

esp_err_t szpi_wifi_ap_netif_prepare(esp_netif_t *netif, const char **stage)
{
    if (!netif || !stage) return ESP_ERR_INVALID_ARG;
    *stage = "DHCP stop";
    esp_err_t err = esp_netif_dhcps_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
    const esp_netif_ip_info_t ip = {
        .ip.addr = ESP_IP4TOADDR(192, 168, 4, 1),
        .gw.addr = ESP_IP4TOADDR(192, 168, 4, 1),
        .netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0),
    };
    *stage = "AP IP";
    err = esp_netif_set_ip_info(netif, &ip);
    if (err != ESP_OK) return err;
    esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4.addr = ip.ip.addr};
    *stage = "AP DNS";
    err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) return err;
    *stage = "DHCP DNS option";
    dhcps_offer_t offer_dns = OFFER_DNS;
    return esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
}
