#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
#include "esp_wifi.h"
#include "szpi_wifi.h"

#define TAG "szpi_wifi"

static esp_netif_t *s_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static szpi_wifi_event_sink_t s_sink;
static void *s_context;
static bool s_wifi_initialized;
static bool s_started;

static void emit_event(szpi_wifi_event_id_t id, int reason, const esp_netif_ip_info_t *ip)
{
    if (s_sink == NULL) return;
    szpi_wifi_event_t event = {.id = id, .disconnect_reason = reason};
    if (ip != NULL) event.ip_info = *ip;
    s_sink(&event, s_context);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START: emit_event(SZPI_WIFI_EVENT_STARTED, 0, NULL); break;
        case WIFI_EVENT_STA_CONNECTED: emit_event(SZPI_WIFI_EVENT_CONNECTED, 0, NULL); break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *event = data;
            emit_event(SZPI_WIFI_EVENT_DISCONNECTED, event != NULL ? event->reason : 0, NULL);
            break;
        }
        case WIFI_EVENT_STA_STOP: emit_event(SZPI_WIFI_EVENT_STOPPED, 0, NULL); break;
        default: break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            const ip_event_got_ip_t *event = data;
            emit_event(SZPI_WIFI_EVENT_GOT_IP, 0, event != NULL ? &event->ip_info : NULL);
        } else if (id == IP_EVENT_STA_LOST_IP) {
            emit_event(SZPI_WIFI_EVENT_LOST_IP, 0, NULL);
        }
    }
}

esp_err_t szpi_wifi_init(const szpi_wifi_config_t *config, szpi_wifi_event_sink_t sink, void *context)
{
    if (s_wifi_initialized) return ESP_ERR_INVALID_STATE;
    if (config == NULL || sink == NULL) return ESP_ERR_INVALID_ARG;
    size_t ssid_len = strnlen(config->ssid, sizeof(config->ssid));
    size_t password_len = strnlen(config->password, sizeof(config->password));
    if (ssid_len == 0 || ssid_len > SZPI_WIFI_SSID_MAX || password_len < 8 || password_len > SZPI_WIFI_PASSWORD_MAX) return ESP_ERR_INVALID_ARG;
    s_sink = sink;
    s_context = context;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) { err = ESP_ERR_NO_MEM; goto fail; }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) goto fail;
    s_wifi_initialized = true;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &s_wifi_handler);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &s_ip_handler);
    if (err != ESP_OK) goto fail;

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, config->ssid, ssid_len);
    memcpy(wifi_config.sta.password, config->password, password_len);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) goto fail;
    return ESP_OK;

fail:
    (void)szpi_wifi_deinit();
    return err;
}

esp_err_t szpi_wifi_start(void)
{
    if (!s_wifi_initialized) return ESP_ERR_INVALID_STATE;
    if (s_started) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) s_started = true;
    return err;
}

esp_err_t szpi_wifi_connect(void) { return s_started ? esp_wifi_connect() : ESP_ERR_INVALID_STATE; }
esp_err_t szpi_wifi_disconnect(void) { return s_started ? esp_wifi_disconnect() : ESP_ERR_INVALID_STATE; }

esp_err_t szpi_wifi_stop(void)
{
    if (!s_started) return ESP_OK;
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_OK) s_started = false;
    return err;
}

esp_err_t szpi_wifi_deinit(void)
{
    esp_err_t result = ESP_OK;
    if (s_started) { esp_err_t err = szpi_wifi_stop(); if (result == ESP_OK) result = err; }
    if (s_ip_handler) { esp_err_t err = esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_handler); if (result == ESP_OK) result = err; s_ip_handler = NULL; }
    if (s_wifi_handler) { esp_err_t err = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler); if (result == ESP_OK) result = err; s_wifi_handler = NULL; }
    if (s_wifi_initialized) { esp_err_t err = esp_wifi_deinit(); if (result == ESP_OK) result = err; s_wifi_initialized = false; }
    if (s_netif != NULL) { esp_netif_destroy_default_wifi(s_netif); s_netif = NULL; }
    s_sink = NULL;
    s_context = NULL;
    return result;
}

esp_err_t szpi_wifi_get_rssi(int8_t *rssi)
{
    if (rssi == NULL) return ESP_ERR_INVALID_ARG;
    wifi_ap_record_t ap = {0};
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err == ESP_OK) *rssi = ap.rssi;
    return err;
}
