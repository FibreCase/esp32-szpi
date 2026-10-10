#include <assert.h>
#include <stdio.h>
#include <string.h>
#define SZPI_RUNTIME_INTERNAL_H
#define SZPI_EVENT_TIME_SYNCED 1U
static void *szpi_system_events;
static unsigned s_sync_bits;
static void xEventGroupSetBits(void *group, unsigned bits) { (void)group; s_sync_bits |= bits; }
#include "../components/szpi_app/src/time_sync_service.c"

static unsigned s_init_count, s_restart_count, s_deinit_count;
static esp_err_t s_init_result;
static esp_err_t s_restart_result;
static const char *s_server;
static void (*s_callback)(struct timeval *);

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    assert(!config->wait_for_sync);
    assert(config->sync_cb);
    s_init_count++;
    s_server = config->servers[0];
    s_callback = config->sync_cb;
    return s_init_result;
}
esp_err_t esp_netif_sntp_start(void) { s_restart_count++; return s_restart_result; }
void esp_netif_sntp_deinit(void) { s_deinit_count++; }

int main(void)
{
    esp_netif_ip_info_t ip = {0};
    uint8_t gateway[] = {192, 168, 1, 1};
    memcpy(&ip.gw.addr, gateway, sizeof(gateway));
#if !CONFIG_SZPI_SNTP_SERVER_GATEWAY
    if (CONFIG_SZPI_SNTP_SERVER_ADDRESS[0] == '\0') {
        assert(szpi_time_sync_request(&ip) == ESP_ERR_INVALID_ARG);
        assert(s_init_count == 0);
        puts("Empty SNTP address rejected: PASS");
        return 0;
    }
#else
    assert(szpi_time_sync_request(NULL) == ESP_ERR_INVALID_STATE);
    esp_netif_ip_info_t missing = {0};
    assert(szpi_time_sync_request(&missing) == ESP_ERR_INVALID_STATE);
    assert(s_init_count == 0);
#endif
    s_init_result = ESP_FAIL;
    assert(szpi_time_sync_request(&ip) == ESP_FAIL);
    assert(!s_sntp_initialized);
    s_init_result = ESP_OK;
    assert(szpi_time_sync_request(&ip) == ESP_OK);
    assert(s_init_count == 2 && s_sntp_initialized);
#if CONFIG_SZPI_SNTP_SERVER_GATEWAY
    assert(!strcmp(s_server, "192.168.1.1"));
#else
    assert(!strcmp(s_server, CONFIG_SZPI_SNTP_SERVER_ADDRESS));
#endif
    assert(szpi_time_sync_request(&ip) == ESP_OK);
    assert(s_restart_count == 1 && s_init_count == 2);
    s_restart_result = ESP_FAIL;
    assert(szpi_time_sync_request(&ip) == ESP_FAIL);
    s_restart_result = ESP_OK;
    gateway[2] = 2;
    memcpy(&ip.gw.addr, gateway, sizeof(gateway));
    assert(szpi_time_sync_request(&ip) == ESP_OK);
#if CONFIG_SZPI_SNTP_SERVER_GATEWAY
    assert(s_deinit_count == 1 && s_init_count == 3);
    assert(!strcmp(s_server, "192.168.2.1"));
    assert(szpi_time_sync_request(&missing) == ESP_ERR_INVALID_STATE);
    assert(s_deinit_count == 2 && !s_sntp_initialized);
    s_init_result = ESP_FAIL;
    assert(szpi_time_sync_request(&ip) == ESP_FAIL);
    assert(!s_sntp_initialized);
    s_init_result = ESP_OK;
    assert(szpi_time_sync_request(&ip) == ESP_OK);
    assert(s_init_count == 5 && s_deinit_count == 2);
#else
    assert(s_deinit_count == 0 && s_init_count == 2);
    assert(!strcmp(s_server, CONFIG_SZPI_SNTP_SERVER_ADDRESS));
#endif
    assert(s_sync_bits == 0);
    s_callback(NULL);
    assert(s_sync_bits == SZPI_EVENT_TIME_SYNCED);
    puts("SNTP selection, retry, gateway change and sync callback: PASS");
}
