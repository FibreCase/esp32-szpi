#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "szpi_app.h"
#include "szpi_runtime_internal.h"

#define TAG "szpi_wifi_svc"
#define ATTEMPT_US (15000LL * 1000)
#define MAX_ATTEMPTS 5
#define SUCCESS_GRACE_US (5000000LL)

static bool s_initialized, s_started, s_sntp_initialized;
static bool s_have_config, s_testing, s_disconnect_pending, s_link_seen;
static bool s_scan_resume_dpp, s_accept_dpp, s_dpp_selected;
static szpi_wifi_config_t s_saved, s_candidate;
static szpi_provision_status_t s_prov;
static szpi_wifi_service_state_t s_state = SZPI_WIFI_STOPPED;
static uint32_t s_attempts, s_dpp_failures;
static int64_t s_deadline, s_retry_at, s_session_deadline, s_close_at;
static esp_err_t s_last_error;

static void time_sync_callback(struct timeval *tv)
{
    (void)tv;
    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_TIME_SYNCED);
}

static void start_time_sync(void)
{
    if (s_sntp_initialized) return;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.wait_for_sync = false;
    config.sync_cb = time_sync_callback;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) s_sntp_initialized = true;
    else ESP_LOGW(TAG, "SNTP unavailable: %s", esp_err_to_name(err));
}

static void publish(void)
{
    if (xSemaphoreTake(szpi_wifi_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        szpi_wifi_status.state = s_state;
        szpi_wifi_status.last_error = s_last_error;
        szpi_wifi_status.attempts = s_attempts;
        szpi_wifi_status.has_config = s_have_config;
        memcpy(szpi_wifi_status.ssid, s_saved.ssid, sizeof(szpi_wifi_status.ssid));
        szpi_wifi_status.provisioning = s_prov;
        if (s_state != SZPI_WIFI_ONLINE) {
            memset(&szpi_wifi_status.ip_info, 0, sizeof(szpi_wifi_status.ip_info));
            szpi_wifi_status.rssi_valid = false;
        }
        xSemaphoreGive(szpi_wifi_status_lock);
    }
    if (s_state == SZPI_WIFI_ONLINE) xEventGroupSetBits(szpi_system_events, SZPI_EVENT_NETWORK_READY);
    else xEventGroupClearBits(szpi_system_events, SZPI_EVENT_NETWORK_READY);
}

static void message_text(const char *text)
{
    snprintf(s_prov.message, sizeof(s_prov.message), "%s", text);
}

void szpi_wifi_post_event(const szpi_wifi_event_t *event, void *context)
{
    (void)context;
    if (!event) return;
    /* Called only by szpi_wifi's serialized sys_evt callback. The queue copies
     * the complete message synchronously; no staging pointer escapes. */
    static wifi_message_t message;
    memset(&message, 0, sizeof(message));
    message.kind = WIFI_MSG_EVENT;
    message.event = *event;
    if (xQueueSend(szpi_wifi_queue, &message, 0) != pdTRUE) {
        if (xSemaphoreTake(szpi_wifi_status_lock, 0) == pdTRUE) {
            szpi_wifi_status.event_queue_overflows++;
            xSemaphoreGive(szpi_wifi_status_lock);
        }
        xEventGroupSetBits(szpi_system_events, SZPI_EVENT_WIFI_OVERFLOW);
    }
    memset(&message, 0, sizeof(message));
    uint32_t minimum = uxTaskGetStackHighWaterMark(NULL);
    if (xSemaphoreTake(szpi_wifi_status_lock, 0) == pdTRUE) {
        if (!szpi_wifi_status.sys_evt_stack_min_bytes || minimum < szpi_wifi_status.sys_evt_stack_min_bytes) {
            szpi_wifi_status.sys_evt_stack_min_bytes = minimum;
        }
        xSemaphoreGive(szpi_wifi_status_lock);
    }
    szpi_runtime_record_queue_peaks();
}

static esp_err_t portal_status(szpi_provision_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(szpi_wifi_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *status = szpi_wifi_status.provisioning;
    xSemaphoreGive(szpi_wifi_status_lock);
    return ESP_OK;
}

static esp_err_t portal_request(szpi_portal_request_t request, uint32_t generation, const szpi_wifi_config_t *config)
{
    if (xSemaphoreTake(szpi_wifi_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    szpi_provision_status_t *status = &szpi_wifi_status.provisioning;
    if (!status->active || status->generation != generation ||
        (status->state != SZPI_PROV_ACTIVE && status->state != SZPI_PROV_FAILED) || status->scanning) {
        xSemaphoreGive(szpi_wifi_status_lock);
        return ESP_ERR_INVALID_STATE;
    }
    wifi_message_t message = {.kind = request == SZPI_PORTAL_SUBMIT ? WIFI_MSG_SUBMIT : WIFI_MSG_SCAN, .generation = generation};
    if (config) message.config = *config;
    bool queued = xQueueSend(szpi_wifi_queue, &message, 0) == pdTRUE;
    if (queued) {
        if (request == SZPI_PORTAL_SUBMIT) status->state = SZPI_PROV_TESTING;
        else status->scanning = true;
    }
    xSemaphoreGive(szpi_wifi_status_lock);
    memset(&message, 0, sizeof(message));
    return queued ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t ensure_driver(void)
{
    esp_err_t err = ESP_OK;
    if (!s_initialized) {
        err = szpi_wifi_init(NULL, szpi_wifi_post_event, NULL);
        if (err == ESP_OK) s_initialized = true;
    }
    if (err == ESP_OK && !s_started) {
        err = szpi_wifi_start();
        if (err == ESP_OK) s_started = true;
    }
    return err;
}

static void connect_station(void)
{
    const szpi_wifi_config_t *config = s_testing ? &s_candidate : &s_saved;
    esp_err_t err = szpi_wifi_set_station(config);
    s_link_seen = false;
    if (err == ESP_OK) err = szpi_wifi_connect();
    s_attempts++;
    s_state = SZPI_WIFI_CONNECTING;
    s_deadline = esp_timer_get_time() + ATTEMPT_US;
    s_last_error = err;
    ESP_LOGI(TAG, "STA connect attempt=%lu candidate=%d result=%s", (unsigned long)s_attempts, s_testing, esp_err_to_name(err));
    if (err != ESP_OK) s_deadline = esp_timer_get_time();
    publish();
}

static void saved_round(void)
{
    s_attempts = 0;
    s_retry_at = 0;
    s_deadline = 0;
    if (!s_have_config) { s_state = SZPI_WIFI_NO_CONFIG; publish(); return; }
    esp_err_t err = ensure_driver();
    if (err != ESP_OK) { s_state = SZPI_WIFI_FAILED; s_last_error = err; publish(); return; }
    connect_station();
}

static void close_session(bool restore)
{
    bool committed = s_prov.state == SZPI_PROV_SUCCESS;
    s_testing = false;
    s_disconnect_pending = false;
    s_deadline = s_retry_at = s_close_at = s_session_deadline = 0;
    esp_err_t err = szpi_wifi_provision_stop();
    if (err != ESP_OK) {
        /* Keep ownership and visible active state until teardown succeeds. */
        s_prov.state = SZPI_PROV_ERROR;
        s_last_error = err;
        message_text("Setup could not stop. Swipe back and retry.");
        publish();
        return;
    }
    uint32_t generation = s_prov.generation;
    memset(&s_prov, 0, sizeof(s_prov));
    s_prov.generation = generation;
    memset(&s_candidate, 0, sizeof(s_candidate));
    if (committed) { s_state = SZPI_WIFI_ONLINE; publish(); return; }
    if (s_started) {
        err = szpi_wifi_stop();
        if (err != ESP_OK) { s_state = SZPI_WIFI_FAILED; s_last_error = err; publish(); return; }
        s_started = false;
    }
    s_state = SZPI_WIFI_STOPPED;
    publish();
    if (restore) saved_round();
}

static void begin_session(bool use_dpp, uint32_t request_id)
{
    if (!request_id) return;
    if (s_prov.active) {
        if (s_prov.request_id == request_id) return;
        close_session(false);
        if (s_prov.active) return;
    }
    s_dpp_selected = use_dpp;
    s_deadline = s_retry_at = 0;
    s_testing = s_disconnect_pending = false;
    /* Stop establishes a clean boundary before changing AP/STA ownership. */
    esp_err_t err = s_started ? szpi_wifi_stop() : ESP_OK;
    if (err != ESP_OK) { s_last_error = err; s_state = SZPI_WIFI_FAILED; publish(); return; }
    s_started = false;
    err = ensure_driver();
    if (err != ESP_OK) { s_last_error = err; s_state = SZPI_WIFI_FAILED; publish(); return; }
    uint32_t generation = s_prov.generation + 1;
    memset(&s_prov, 0, sizeof(s_prov));
    s_prov.generation = generation ? generation : 1;
    s_prov.request_id = request_id;
    s_prov.active = true;
    s_prov.state = SZPI_PROV_ACTIVE;
    s_dpp_failures = 0;
    s_accept_dpp = use_dpp;
    message_text(use_dpp ? "Preparing Easy Connect..." : "Preparing Wi-Fi Hotspot...");
    err = szpi_wifi_provision_start(&s_prov, use_dpp, portal_request, portal_status);
    if (err != ESP_OK) {
        s_prov.active = false;
        s_prov.state = SZPI_PROV_ERROR;
        s_last_error = err;
        ESP_LOGE(TAG, "provisioning start failed: %s", esp_err_to_name(err));
        if (!s_prov.message[0]) message_text("Setup unavailable. Try again.");
        s_state = SZPI_WIFI_FAILED;
        publish();
        if (szpi_wifi_stop() == ESP_OK) { s_started = false; saved_round(); }
        return;
    }
    s_session_deadline = esp_timer_get_time() + (int64_t)CONFIG_SZPI_WIFI_PROVISION_TIMEOUT_S * 1000000;
    s_state = SZPI_WIFI_NO_CONFIG;
    publish();
}

static void test_failed(esp_err_t cause)
{
    ESP_LOGW(TAG, "candidate connection failed: %s", esp_err_to_name(cause));
    int8_t rssi;
    if (szpi_wifi_get_rssi(&rssi) == ESP_OK) (void)szpi_wifi_disconnect();
    s_testing = s_disconnect_pending = false;
    s_deadline = 0;
    s_state = SZPI_WIFI_FAILED;
    s_last_error = cause;
    s_prov.state = SZPI_PROV_FAILED;
    memset(&s_candidate, 0, sizeof(s_candidate));
    message_text(cause == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? "Cannot save settings. Old network preserved." :
        "Connection failed. Check password and 2.4 GHz network; try again.");
    publish();
}

static void submit_candidate(const szpi_wifi_config_t *config)
{
    if (s_testing || s_prov.scanning || !s_prov.active || s_prov.state == SZPI_PROV_SUCCESS) return;
    s_accept_dpp = false;
    esp_err_t err = szpi_wifi_dpp_stop();
    s_prov.dpp_ready = false;
    if (err != ESP_OK) { test_failed(err); return; }
    s_candidate = *config;
    s_testing = true;
    s_prov.state = SZPI_PROV_TESTING;
    message_text("Connecting and waiting for an IP address...");
    s_attempts = 0;
    int8_t rssi;
    bool linked = szpi_wifi_get_rssi(&rssi) == ESP_OK;
    err = linked ? szpi_wifi_disconnect() : ESP_ERR_WIFI_NOT_CONNECT;
    if (!linked && (err == ESP_OK || err == ESP_ERR_WIFI_NOT_CONNECT)) {
        s_disconnect_pending = false;
        connect_station();
    } else if (err == ESP_OK) {
        s_disconnect_pending = true;
        s_link_seen = false;
        s_deadline = esp_timer_get_time() + 2000000;
        s_state = SZPI_WIFI_CONNECTING;
        publish();
    } else if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        s_disconnect_pending = false;
        connect_station();
    } else test_failed(err);
}

static void event_received(const szpi_wifi_event_t *event)
{
    if (event->id == SZPI_WIFI_EVENT_DPP_URI || event->id == SZPI_WIFI_EVENT_DPP_CONFIG || event->id == SZPI_WIFI_EVENT_DPP_FAILED) {
        if (!s_prov.active || !s_accept_dpp || s_testing || s_prov.state == SZPI_PROV_SUCCESS || event->generation != s_prov.generation) return;
        if (event->id == SZPI_WIFI_EVENT_DPP_URI) {
            memcpy(s_prov.dpp_uri, event->uri, sizeof(s_prov.dpp_uri));
            esp_err_t err = s_dpp_selected ? szpi_wifi_dpp_listen() : szpi_wifi_dpp_pause();
            s_prov.dpp_ready = s_dpp_selected && err == ESP_OK;
            if (err != ESP_OK) message_text("Easy Connect unavailable. Use Wi-Fi Hotspot.");
            publish();
        } else if (event->id == SZPI_WIFI_EVENT_DPP_CONFIG) {
            if (!s_dpp_selected) return;
            ESP_LOGI(TAG, "DPP credentials received; testing station (SSID bytes=%u, WPA3-only=%d)",
                (unsigned)strlen(event->config.ssid), event->config.wpa3_only);
            submit_candidate(&event->config);
        } else {
            if (!s_dpp_selected) return;
            ESP_LOGW(TAG, "DPP exchange failed: %s", esp_err_to_name(event->disconnect_reason));
            s_dpp_failures++;
            esp_err_t err = s_dpp_failures <= 3 ? szpi_wifi_dpp_listen() : ESP_FAIL;
            if (err != ESP_OK) {
                (void)szpi_wifi_dpp_stop();
                s_prov.dpp_ready = false;
            }
            message_text("Easy Connect failed. Retry or use Wi-Fi Hotspot.");
            publish();
        }
        return;
    }
    if (event->id == SZPI_WIFI_EVENT_SCAN_DONE) {
        if (!s_prov.active || !s_prov.scanning) return;
        esp_err_t err = szpi_wifi_scan_results(&s_prov);
        s_prov.scanning = false;
        if (s_scan_resume_dpp) {
            s_prov.dpp_ready = szpi_wifi_dpp_listen() == ESP_OK;
            s_scan_resume_dpp = false;
        }
        if (err != ESP_OK) {
            message_text("Scan failed. Retry or enter the network manually.");
        } else if (!s_prov.scan_count) {
            message_text("Scan complete. No networks found. Retry or enter a hidden SSID.");
        } else {
            char summary[sizeof(s_prov.message)];
            snprintf(summary, sizeof(summary), "Scan complete. %u network%s found. Choose one below or enter a hidden SSID.",
                s_prov.scan_count, s_prov.scan_count == 1 ? "" : "s");
            message_text(summary);
        }
        publish();
        return;
    }
    if (s_state == SZPI_WIFI_STOPPED || s_state == SZPI_WIFI_STOPPING || !s_started) return;
    if (event->id == SZPI_WIFI_EVENT_CONNECTED) {
        if (!s_disconnect_pending && s_deadline) s_link_seen = true;
    } else if (event->id == SZPI_WIFI_EVENT_GOT_IP) {
        if (s_disconnect_pending || !s_link_seen || (!s_deadline && s_state != SZPI_WIFI_ONLINE)) return;
        const szpi_wifi_config_t *config = s_testing ? &s_candidate : &s_saved;
        if (szpi_wifi_verify_station(config) != ESP_OK) return;
        if (s_testing) {
            esp_err_t err = szpi_wifi_save_config(&s_candidate);
            if (err != ESP_OK) {
                test_failed(err);
                message_text("Cannot save settings. Old network preserved. Retry or swipe back.");
                publish();
                return;
            }
            s_saved = s_candidate;
            memset(&s_candidate, 0, sizeof(s_candidate));
            s_have_config = true;
            s_testing = false;
            s_prov.state = SZPI_PROV_SUCCESS;
            message_text("Connected! Settings saved. Hotspot closes in 5 seconds.");
            s_close_at = esp_timer_get_time() + SUCCESS_GRACE_US;
        }
        s_deadline = s_retry_at = 0;
        s_attempts = 0;
        s_last_error = ESP_OK;
        s_state = SZPI_WIFI_ONLINE;
        publish();
        if (xSemaphoreTake(szpi_wifi_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            szpi_wifi_status.ip_info = event->ip_info;
            xSemaphoreGive(szpi_wifi_status_lock);
        }
        start_time_sync();
    } else if (event->id == SZPI_WIFI_EVENT_DISCONNECTED || event->id == SZPI_WIFI_EVENT_LOST_IP) {
        ESP_LOGW(TAG, "STA link lost: event=%d reason=%d candidate=%d", event->id, event->disconnect_reason, s_testing);
        if (xSemaphoreTake(szpi_wifi_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            szpi_wifi_status.last_disconnect_reason = event->disconnect_reason;
            xSemaphoreGive(szpi_wifi_status_lock);
        }
        if (s_disconnect_pending && event->id == SZPI_WIFI_EVENT_DISCONNECTED) {
            s_disconnect_pending = false;
            connect_station();
        } else if (s_testing) {
            test_failed(ESP_FAIL);
            if (event->disconnect_reason == WIFI_REASON_NO_AP_FOUND) {
                message_text("Network not found. Enable 2.4 GHz on the target AP, then try again.");
                publish();
            }
        }
        else if (!s_prov.active && s_have_config && (s_deadline || s_state == SZPI_WIFI_ONLINE)) {
            s_link_seen = false;
            s_deadline = esp_timer_get_time();
            s_state = SZPI_WIFI_RETRY_WAIT;
            publish();
        }
    }
}

static void stop_driver(void)
{
    if (s_prov.active) close_session(false);
    esp_err_t err = s_prov.active ? ESP_ERR_INVALID_STATE : (s_started ? szpi_wifi_stop() : ESP_OK);
    if (err == ESP_OK) s_started = false;
    s_deadline = s_retry_at = 0;
    s_state = err == ESP_OK ? SZPI_WIFI_STOPPED : SZPI_WIFI_FAILED;
    s_last_error = err;
    publish();
    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_WIFI_STOPPED);
}

void szpi_wifi_service_task(void *context)
{
    (void)context;
    if (setenv("TZ", "CST-8", 1) == 0) tzset();
    (void)xEventGroupWaitBits(szpi_system_events, SZPI_EVENT_RUNTIME_START, pdFALSE, pdTRUE, portMAX_DELAY);
    for (;;) {
        wifi_message_t message;
        if (xQueueReceive(szpi_wifi_queue, &message, pdMS_TO_TICKS(50)) == pdTRUE) {
            switch (message.kind) {
            case WIFI_MSG_START: {
                esp_err_t err = szpi_wifi_load_config(&s_saved);
                s_have_config = err == ESP_OK;
                if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
                    /* Corruption/read failures are visible and never trigger NVS erase. */
                    ESP_LOGW(TAG, "saved Wi-Fi unavailable: %s", esp_err_to_name(err));
                    s_last_error = err;
                }
                if (s_have_config) saved_round();
                else saved_round();
                break;
            }
            case WIFI_MSG_STOP: stop_driver(); break;
            case WIFI_MSG_RETRY: if (!s_prov.active) saved_round(); break;
            case WIFI_MSG_PROVISION: begin_session(message.use_dpp, message.request_id); break;
            case WIFI_MSG_CANCEL:
                if (s_prov.active && message.request_id == s_prov.request_id) close_session(true);
                break;
            case WIFI_MSG_FORGET:
                if (!s_prov.active && message.generation == s_prov.generation) {
                    esp_err_t err = szpi_wifi_forget_config();
                    if (err == ESP_OK) {
                        memset(&s_saved, 0, sizeof(s_saved));
                        s_have_config = false;
                        stop_driver();
                        s_state = SZPI_WIFI_NO_CONFIG;
                        message_text("Choose Connect a New Network to set up Wi-Fi.");
                        publish();
                    } else { s_last_error = err; message_text("Cannot clear settings. Try again."); publish(); }
                }
                break;
            case WIFI_MSG_SUBMIT:
                if (message.generation == s_prov.generation) submit_candidate(&message.config);
                break;
            case WIFI_MSG_SCAN:
                if (s_prov.active && !s_testing && !s_prov.scanning && message.generation == s_prov.generation) {
                    s_scan_resume_dpp = s_prov.dpp_ready;
                    if (s_scan_resume_dpp) {
                        /* Stop listening without dropping bootstrap/session keys. */
                        (void)szpi_wifi_dpp_pause();
                        s_prov.dpp_ready = false;
                    }
                    esp_err_t err = szpi_wifi_scan_start();
                    s_prov.scanning = err == ESP_OK;
                    if (err == ESP_OK) message_text("Scanning nearby networks...");
                    if (err != ESP_OK) {
                        if (s_scan_resume_dpp) s_prov.dpp_ready = szpi_wifi_dpp_listen() == ESP_OK;
                        s_scan_resume_dpp = false;
                        message_text("Scan unavailable. Enter the network manually.");
                    }
                    publish();
                }
                break;
            case WIFI_MSG_EVENT: event_received(&message.event); break;
            default: break;
            }
            memset(&message, 0, sizeof(message));
        }
        szpi_wifi_portal_poll();
        int64_t now = esp_timer_get_time();
        if (s_close_at && now >= s_close_at) close_session(true);
        else if (s_session_deadline && now >= s_session_deadline && s_prov.state != SZPI_PROV_SUCCESS) close_session(true);
        if ((xEventGroupGetBits(szpi_system_events) & SZPI_EVENT_WIFI_OVERFLOW) != 0) {
            xEventGroupClearBits(szpi_system_events, SZPI_EVENT_WIFI_OVERFLOW);
            if (s_testing) test_failed(ESP_ERR_TIMEOUT);
            else if (!s_prov.active && s_have_config && s_started) s_deadline = now;
        }
        if (s_deadline && now >= s_deadline) {
            if (s_testing) test_failed(ESP_ERR_TIMEOUT);
            else {
                s_deadline = 0;
                (void)szpi_wifi_disconnect();
                if (s_attempts >= MAX_ATTEMPTS) {
                    s_state = SZPI_WIFI_FAILED;
                    s_last_error = ESP_ERR_TIMEOUT;
                    publish();
                } else {
                    s_state = SZPI_WIFI_RETRY_WAIT;
                    s_retry_at = now + (1LL << (s_attempts ? s_attempts - 1 : 0)) * 1000000;
                    publish();
                }
            }
        }
        if (s_retry_at && now >= s_retry_at) { s_retry_at = 0; connect_station(); }
    }
}
