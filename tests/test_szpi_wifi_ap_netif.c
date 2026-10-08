#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wifi_ap_netif.h"
static enum { INITIAL, STOPPED, STARTED } dhcp_state;
static unsigned step, fail_step;
esp_err_t esp_netif_dhcps_stop(esp_netif_t *netif)
{
    assert(netif && ++step == 1);
    if (fail_step == step) return ESP_FAIL;
    if (dhcp_state == STOPPED) return ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED;
    dhcp_state = STOPPED;
    return ESP_OK;
}
esp_err_t esp_netif_set_ip_info(esp_netif_t *netif, const esp_netif_ip_info_t *ip)
{
    assert(netif && ++step == 2);
    if (dhcp_state != STOPPED) return ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED;
    if (fail_step == step) return ESP_FAIL;
    assert(ip->ip.addr == ESP_IP4TOADDR(192,168,4,1));
    assert(ip->netmask.addr == ESP_IP4TOADDR(255,255,255,0));
    return ESP_OK;
}
esp_err_t esp_netif_set_dns_info(esp_netif_t *netif, int type, esp_netif_dns_info_t *dns)
{
    assert(netif && ++step == 3 && type == ESP_NETIF_DNS_MAIN && dhcp_state == STOPPED);
    if (fail_step == step) return ESP_FAIL;
    assert(dns->ip.u_addr.ip4.addr == ESP_IP4TOADDR(192,168,4,1));
    return ESP_OK;
}
esp_err_t esp_netif_dhcps_option(esp_netif_t *netif, int op, int option, void *value, uint32_t size)
{
    assert(netif && ++step == 4 && dhcp_state == STOPPED);
    assert(op == ESP_NETIF_OP_SET && option == ESP_NETIF_DOMAIN_NAME_SERVER);
    assert(size == 1 && *(uint8_t *)value == DHCPS_OFFER_DNS);
    return fail_step == step ? ESP_FAIL : ESP_OK;
}
int main(void)
{
    esp_netif_t netif = {0};
    const char *stage;
    for (unsigned state = INITIAL; state <= STARTED; state++) {
        dhcp_state = state; step = fail_step = 0;
        assert(szpi_wifi_ap_netif_prepare(&netif, &stage) == ESP_OK);
        assert(step == 4 && dhcp_state == STOPPED);
    }
    const char *stages[] = {"DHCP stop", "AP IP", "AP DNS", "DHCP DNS option"};
    for (fail_step = 1; fail_step <= 4; fail_step++) {
        dhcp_state = INITIAL; step = 0;
        assert(szpi_wifi_ap_netif_prepare(&netif, &stage) == ESP_FAIL);
        assert(step == fail_step && !strcmp(stage, stages[fail_step - 1]));
    }
    puts("AP DHCP lifecycle and failure stages: PASS");
}
