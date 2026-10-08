#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "esp_dpp.h"
#include "esp_mac.h"
#include "lwip/inet.h"
#include "esp_random.h"
#include "provision_protocol.h"
#include "wifi_portal.h"
#include "wifi_ap_netif.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "szpi_wifi.h"

#define TAG "szpi_wifi"

static esp_netif_t *s_netif;
static esp_netif_t *s_ap_netif;
static char s_hostname[SZPI_WIFI_HOSTNAME_MAX + 1] = SZPI_WIFI_HOSTNAME_DEFAULT;
static bool s_dpp_initialized;
static atomic_uint s_generation;
/* Default-loop events publish association state to read-only status queries. */
static atomic_bool s_associated;
ESP_EVENT_DEFINE_BASE(SZPI_WIFI_BARRIER);
static esp_event_handler_instance_t s_barrier_handler;
static StaticSemaphore_t s_barrier_storage;
static SemaphoreHandle_t s_barrier;
static uint32_t s_barrier_sequence;
static atomic_uint s_barrier_ack;

static void barrier_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    if (data) atomic_store(&s_barrier_ack, *(const uint32_t *)data);
    xSemaphoreGive(s_barrier);
}

/* DPP producer is stopped first. Drain default-loop callbacks before the next
 * generation so old callbacks cannot be stamped with a new session token. */
static esp_err_t drain_events(void)
{
    if (!s_barrier_handler) return ESP_OK;
    (void)xSemaphoreTake(s_barrier, 0);
    uint32_t sequence = ++s_barrier_sequence;
    esp_err_t err = esp_event_post(SZPI_WIFI_BARRIER, 0, &sequence, sizeof(sequence), pdMS_TO_TICKS(1000));
    if (err != ESP_OK) return err;
    return xSemaphoreTake(s_barrier, pdMS_TO_TICKS(1000)) == pdTRUE && atomic_load(&s_barrier_ack) == sequence ? ESP_OK : ESP_ERR_TIMEOUT;
}
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static szpi_wifi_event_sink_t s_sink;
static void *s_context;
static bool s_wifi_initialized;
static bool s_started;
/* Sole writer is the serialized default event loop. Sink copies by value
 * before returning; neither callbacks nor queues retain this pointer. */
static szpi_wifi_event_t s_callback_event;

static szpi_wifi_event_t *callback_event(szpi_wifi_event_id_t id)
{
    memset(&s_callback_event, 0, sizeof(s_callback_event));
    s_callback_event.id = id;
    s_callback_event.generation = atomic_load(&s_generation);
    return &s_callback_event;
}

static void emit_event(szpi_wifi_event_id_t id, int reason, const esp_netif_ip_info_t *ip)
{
    if (s_sink == NULL) return;
    szpi_wifi_event_t *event = callback_event(id);
    event->disconnect_reason = reason;
    if (ip != NULL) event->ip_info = *ip;
    s_sink(event, s_context);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START: emit_event(SZPI_WIFI_EVENT_STARTED, 0, NULL); break;
        case WIFI_EVENT_STA_CONNECTED:
            atomic_store(&s_associated, true);
            emit_event(SZPI_WIFI_EVENT_CONNECTED, 0, NULL);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            atomic_store(&s_associated, false);
            const wifi_event_sta_disconnected_t *event = data;
            emit_event(SZPI_WIFI_EVENT_DISCONNECTED, event != NULL ? event->reason : 0, NULL);
            break;
        }
        case WIFI_EVENT_STA_STOP:
            atomic_store(&s_associated, false);
            emit_event(SZPI_WIFI_EVENT_STOPPED, 0, NULL);
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t *client = data;
            ESP_LOGI(TAG, "hotspot client associated aid=%u", client ? client->aid : 0);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            const wifi_event_ap_stadisconnected_t *client = data;
            ESP_LOGW(TAG, "hotspot client disconnected reason=%d", client ? client->reason : 0);
            break;
        }
        case WIFI_EVENT_SCAN_DONE: emit_event(SZPI_WIFI_EVENT_SCAN_DONE, 0, NULL); break;
        case WIFI_EVENT_DPP_URI_READY: {
            const wifi_event_dpp_uri_ready_t *uri = data;
            szpi_wifi_event_t *event = callback_event(SZPI_WIFI_EVENT_DPP_URI);
            if (uri && uri->uri_data_len > 0 && uri->uri_data_len <= sizeof(event->uri)) {
                memcpy(event->uri, uri->uri, uri->uri_data_len);
                event->uri[sizeof(event->uri) - 1] = 0;
                if (s_sink) s_sink(event, s_context);
            } else emit_event(SZPI_WIFI_EVENT_DPP_FAILED, ESP_ERR_INVALID_SIZE, NULL);
            break;
        }
        case WIFI_EVENT_DPP_CFG_RECVD: {
            const wifi_event_dpp_config_received_t *received = data;
            bool found = false;
            if (received) for (unsigned i = 0; i < received->total_conf; i++) {
                const esp_dpp_config_data_t *row = &received->configs[i];
                /* Home/office PSK and SAE. Enterprise/DPP connector-only rows
                 * cannot be persisted as passphrases and must not be faked. */
                if (row->akm != ESP_DPP_AKM_PSK && row->akm != ESP_DPP_AKM_SAE && row->akm != ESP_DPP_AKM_PSK_SAE) continue;
                if (!row->ssid_len || row->ssid_len > 32 || row->password_len > 64 ||
                    memchr(row->ssid, 0, row->ssid_len) || memchr(row->password, 0, row->password_len)) continue;
                szpi_wifi_event_t *event = callback_event(SZPI_WIFI_EVENT_DPP_CONFIG);
                memcpy(event->config.ssid, row->ssid, row->ssid_len);
                memcpy(event->config.password, row->password, row->password_len);
                event->config.wpa3_only = row->akm == ESP_DPP_AKM_SAE;
                if (!szpi_wifi_credentials_valid(event->config.ssid, event->config.password, event->config.wpa3_only)) continue;
                if (s_sink) s_sink(event, s_context);
                found = true;
                memset(event, 0, sizeof(*event));
                break;
            }
            if (!found) emit_event(SZPI_WIFI_EVENT_DPP_FAILED, ESP_ERR_NOT_SUPPORTED, NULL);
            break;
        }
        case WIFI_EVENT_DPP_FAILED: {
            const wifi_event_dpp_failed_t *failure = data;
            emit_event(SZPI_WIFI_EVENT_DPP_FAILED, failure ? failure->failure_reason : ESP_FAIL, NULL);
            break;
        }
        default: break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
            const ip_event_assigned_ip_to_client_t *lease = data;
            if (lease && lease->esp_netif == s_ap_netif) ESP_LOGI(TAG, "hotspot DHCP lease=" IPSTR, IP2STR(&lease->ip));
        } else if (id == IP_EVENT_STA_GOT_IP) {
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
    if (sink == NULL) return ESP_ERR_INVALID_ARG;
    if (config && !szpi_wifi_credentials_valid(config->ssid, config->password, config->wpa3_only)) return ESP_ERR_INVALID_ARG;
    s_sink = sink;
    s_context = context;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;

    s_barrier = xSemaphoreCreateBinaryStatic(&s_barrier_storage);
    if (!s_barrier) { err = ESP_ERR_NO_MEM; goto fail; }
    err = esp_event_handler_instance_register(SZPI_WIFI_BARRIER, 0, barrier_handler, NULL, &s_barrier_handler);
    if (err != ESP_OK) goto fail;
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) { err = ESP_ERR_NO_MEM; goto fail; }
    err = szpi_wifi_load_hostname(s_hostname);
    if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "stored hostname is invalid; using default");
        memcpy(s_hostname, SZPI_WIFI_HOSTNAME_DEFAULT, sizeof(SZPI_WIFI_HOSTNAME_DEFAULT));
        err = ESP_OK;
    }
    if (err != ESP_OK) goto fail;
    err = esp_netif_set_hostname(s_netif, s_hostname);
    if (err != ESP_OK) goto fail;
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

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK && config) err = szpi_wifi_set_station(config);
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
    if (err == ESP_OK) {
        s_started = false;
        atomic_store(&s_associated, false);
    }
    return err;
}

esp_err_t szpi_wifi_deinit(void)
{
    esp_err_t result = szpi_wifi_provision_stop();
    if (result != ESP_OK) return result;
    if (s_started) {
        result = szpi_wifi_stop();
        if (result != ESP_OK) return result;
    }
    if (s_barrier_handler) { esp_err_t err = esp_event_handler_instance_unregister(SZPI_WIFI_BARRIER, 0, s_barrier_handler); if (result == ESP_OK) result = err; s_barrier_handler = NULL; }
    if (s_barrier) { vSemaphoreDelete(s_barrier); s_barrier = NULL; }
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
    if (!atomic_load(&s_associated)) return ESP_ERR_WIFI_NOT_CONNECT;
    wifi_ap_record_t ap = {0};
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err == ESP_OK) *rssi = ap.rssi;
    return err;
}

esp_err_t szpi_wifi_set_hostname(const char *hostname)
{
    if (!szpi_wifi_hostname_valid(hostname)) return ESP_ERR_INVALID_ARG;
    if (s_netif != NULL) {
        esp_err_t err = esp_netif_set_hostname(s_netif, hostname);
        if (err != ESP_OK) return err;
    }
    memcpy(s_hostname, hostname, strlen(hostname) + 1);
    return ESP_OK;
}

esp_err_t szpi_wifi_get_hostname(char hostname[SZPI_WIFI_HOSTNAME_MAX + 1])
{
    if (hostname == NULL) return ESP_ERR_INVALID_ARG;
    memcpy(hostname, s_hostname, sizeof(s_hostname));
    return ESP_OK;
}

esp_err_t szpi_wifi_get_link_info(szpi_wifi_link_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    info->mac_valid = esp_read_mac(info->mac, ESP_MAC_WIFI_STA) == ESP_OK;
    if (!atomic_load(&s_associated)) return ESP_OK;
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        info->link_valid = true;
        memcpy(info->ssid, ap.ssid, 32);
        memcpy(info->bssid, ap.bssid, sizeof(info->bssid));
        info->channel = ap.primary;
        info->rssi = ap.rssi;
    }
    return ESP_OK;
}

esp_err_t szpi_wifi_set_station(const szpi_wifi_config_t *config)
{
    if (!config || !szpi_wifi_credentials_valid(config->ssid, config->password, config->wpa3_only)) return ESP_ERR_INVALID_ARG;
    wifi_config_t station = {0};
    memcpy(station.sta.ssid, config->ssid, strlen(config->ssid));
    memcpy(station.sta.password, config->password, strlen(config->password));
    station.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    station.sta.channel = 0;
    station.sta.bssid_set = false;
    station.sta.threshold.authmode = config->wpa3_only ? WIFI_AUTH_WPA3_PSK : WIFI_AUTH_WPA2_PSK;
    station.sta.pmf_cfg.capable = true;
    station.sta.pmf_cfg.required = config->wpa3_only;
    station.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &station);
    memset(&station, 0, sizeof(station));
    return err;
}

esp_err_t szpi_wifi_verify_station(const szpi_wifi_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    wifi_ap_record_t ap = {0};
    esp_netif_ip_info_t ip = {0};
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) return err;
    if (strncmp((const char *)ap.ssid, config->ssid, 32) != 0) return ESP_ERR_INVALID_STATE;
    err = esp_netif_get_ip_info(s_netif, &ip);
    return err == ESP_OK && ip.ip.addr && ip.netmask.addr ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t szpi_wifi_dpp_stop(void)
{
    if (!s_dpp_initialized) return drain_events();
    esp_err_t err = esp_supp_dpp_stop_listen();
    esp_err_t deinit = esp_supp_dpp_deinit();
    if (deinit == ESP_OK) s_dpp_initialized = false;
    if (err != ESP_OK) return err;
    if (deinit != ESP_OK) return deinit;
    return drain_events();
}

esp_err_t szpi_wifi_dpp_pause(void)
{
    return s_dpp_initialized ? esp_supp_dpp_stop_listen() : ESP_OK;
}

esp_err_t szpi_wifi_dpp_listen(void)
{
    return s_dpp_initialized ? esp_supp_dpp_start_listen() : ESP_ERR_INVALID_STATE;
}

esp_err_t szpi_wifi_provision_start(szpi_provision_status_t *status, bool use_dpp,
    szpi_portal_request_cb_t request_cb, szpi_portal_status_cb_t status_cb)
{
    if (!status || !s_started) return ESP_ERR_INVALID_STATE;
    atomic_store(&s_generation, status->generation);
    const char *stage = "DPP init";
    esp_err_t err;
    if (use_dpp) {
        err = esp_supp_dpp_init();
        if (err != ESP_OK) goto fail;
        s_dpp_initialized = true;
        stage = "DPP bootstrap";
        err = esp_supp_dpp_bootstrap_gen("6", DPP_BOOTSTRAP_QR_CODE, NULL, "SZ-PI");
        if (err != ESP_OK) goto fail;
        ESP_LOGI(TAG, "DPP session prepared in STA mode; no hotspot or portal");
        return ESP_OK;
    }
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) return ESP_ERR_NO_MEM;
    }
    stage = "AP netif";
    err = szpi_wifi_ap_netif_prepare(s_ap_netif, &stage);
    if (err != ESP_OK) goto fail;
    stage = "AP MAC";
    uint8_t mac[6];
    err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) goto fail;
    snprintf(status->ap_ssid, sizeof(status->ap_ssid), "Device-%02X%02X%02X", mac[3], mac[4], mac[5]);
    /* Wi-Fi is running, so esp_fill_random has a live RF entropy source. */
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    for (unsigned i = 0; i < 16; i++) status->ap_password[i] = alphabet[random[i] % (sizeof(alphabet) - 1)];
    status->ap_password[16] = 0;
    memset(random, 0, sizeof(random));
    stage = "Wi-Fi QR";
    if (!szpi_wifi_qr_encode(status->wifi_qr, sizeof(status->wifi_qr), status->ap_ssid, status->ap_password)) { err = ESP_ERR_INVALID_SIZE; goto fail; }
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, status->ap_ssid, strlen(status->ap_ssid));
    ap.ap.ssid_len = strlen(status->ap_ssid);
    memcpy(ap.ap.password, status->ap_password, 16);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.pmf_cfg.capable = true;
    /* Configure a stopped radio so AP never starts with default open auth. */
    stage = "radio stop";
    err = szpi_wifi_stop();
    if (err == ESP_OK) { stage = "APSTA mode"; err = esp_wifi_set_mode(WIFI_MODE_APSTA); }
    if (err == ESP_OK) { stage = "AP configuration"; err = esp_wifi_set_config(WIFI_IF_AP, &ap); }
    /* Re-enable DHCP auto-start after explicit stop. While AP is down this
     * changes DHCP state to INIT; the AP_START action starts the actual server. */
    if (err == ESP_OK) { stage = "DHCP enable"; err = esp_netif_dhcps_start(s_ap_netif); }
    if (err == ESP_OK) { stage = "radio start"; err = szpi_wifi_start(); }
    memset(&ap, 0, sizeof(ap));
    if (err != ESP_OK) goto fail;
    stage = "AP ready";
    TickType_t ready_start = xTaskGetTickCount();
    while (!esp_netif_is_netif_up(s_ap_netif) &&
           xTaskGetTickCount() - ready_start < pdMS_TO_TICKS(1500)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!esp_netif_is_netif_up(s_ap_netif)) { err = ESP_ERR_TIMEOUT; goto fail; }
    stage = "DHCP start";
    err = esp_netif_dhcps_start(s_ap_netif);
    if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) err = ESP_OK;
    if (err != ESP_OK) goto fail;
    stage = "DHCP status";
    esp_netif_dhcp_status_t dhcp_status;
    err = esp_netif_dhcps_get_status(s_ap_netif, &dhcp_status);
    if (err != ESP_OK) goto fail;
    if (dhcp_status != ESP_NETIF_DHCP_STARTED) { err = ESP_ERR_INVALID_STATE; goto fail; }
    esp_netif_ip_info_t ready_ip;
    err = esp_netif_get_ip_info(s_ap_netif, &ready_ip);
    if (err != ESP_OK) goto fail;
    ESP_LOGI(TAG, "hotspot ready; DHCP started; IP=" IPSTR, IP2STR(&ready_ip.ip));
    stage = "portal start";
    err = szpi_portal_start(status->generation, request_cb, status_cb);
    if (err != ESP_OK) goto fail;
    ESP_LOGI(TAG, "SoftAP session prepared; DPP disabled");
    return ESP_OK;
fail:
    ESP_LOGE(TAG, "provisioning start failed at %s: %s", stage, esp_err_to_name(err));
    snprintf(status->message, sizeof(status->message), "Setup failed: %s (%s)", stage, esp_err_to_name(err));
    esp_err_t cleanup = szpi_wifi_provision_stop();
    if (cleanup != ESP_OK) ESP_LOGE(TAG, "provisioning rollback failed: %s", esp_err_to_name(cleanup));
    return err;
}

esp_err_t szpi_wifi_provision_stop(void)
{
    esp_err_t err = szpi_wifi_dpp_stop();
    esp_err_t portal_err = szpi_portal_stop();
    if (err == ESP_OK) err = portal_err;
    if (s_wifi_initialized && s_ap_netif) {
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) err = mode_err;
        if (mode_err == ESP_OK) {
            esp_netif_destroy_default_wifi(s_ap_netif);
            s_ap_netif = NULL;
        }
    }
    return err;
}

void szpi_wifi_portal_poll(void) { szpi_portal_poll(); }

esp_err_t szpi_wifi_scan_start(void)
{
    wifi_scan_config_t config = {.show_hidden = true};
    return esp_wifi_scan_start(&config, false);
}

esp_err_t szpi_wifi_scan_results(szpi_provision_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    wifi_ap_record_t records[SZPI_WIFI_SCAN_MAX];
    uint16_t count = SZPI_WIFI_SCAN_MAX;
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, records);
    status->scan_count = 0;
    if (err != ESP_OK) return err;
    for (unsigned i = 0; i < count; i++) {
        if (!records[i].ssid[0]) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < status->scan_count; j++) {
            if (!strncmp(status->scan[j].ssid, (const char *)records[i].ssid, 32)) duplicate = true;
        }
        if (duplicate) continue;
        szpi_wifi_scan_entry_t *entry = &status->scan[status->scan_count++];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->ssid, records[i].ssid, 32);
        entry->rssi = records[i].rssi;
        entry->wpa3_only = records[i].authmode == WIFI_AUTH_WPA3_PSK;
        entry->supported = records[i].authmode == WIFI_AUTH_WPA2_PSK ||
            records[i].authmode == WIFI_AUTH_WPA_WPA2_PSK ||
            records[i].authmode == WIFI_AUTH_WPA2_WPA3_PSK || entry->wpa3_only;
    }
    return ESP_OK;
}
