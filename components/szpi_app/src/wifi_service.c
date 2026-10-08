#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "szpi_app.h"
#include "szpi_runtime_internal.h"

#define TAG "szpi_wifi_svc"
#define ATTEMPT_TIMEOUT_MS 15000
#define ROUND_TIMEOUT_MS 90000
#define MAX_ATTEMPTS 5
static const uint8_t s_retry_delay_s[] = {1, 2, 4, 8};

static bool s_adapter_initialized;
static bool s_driver_started;
static bool s_stopping;
static bool s_sntp_initialized;
static szpi_wifi_config_t s_config;

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
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP initialization failed: %s", esp_err_to_name(err));
        return;
    }
    s_sntp_initialized = true;
    ESP_LOGI(TAG, "SNTP started with pool.ntp.org");
}

void szpi_wifi_service_configure(const szpi_wifi_config_t *config)
{
    if (config != NULL) s_config = *config;
}

static void set_state(szpi_wifi_service_state_t state, esp_err_t err)
{
    if (xSemaphoreTake(szpi_wifi_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        szpi_wifi_status.state = state;
        if (err != ESP_OK) szpi_wifi_status.last_error = err;
        if (state != SZPI_WIFI_ONLINE) {
            memset(&szpi_wifi_status.ip_info, 0, sizeof(szpi_wifi_status.ip_info));
            szpi_wifi_status.rssi_valid = false;
        }
        xSemaphoreGive(szpi_wifi_status_lock);
    }
    if (state == SZPI_WIFI_ONLINE) xEventGroupSetBits(szpi_system_events, SZPI_EVENT_NETWORK_READY);
    else xEventGroupClearBits(szpi_system_events, SZPI_EVENT_NETWORK_READY);
}

void szpi_wifi_post_event(const szpi_wifi_event_t *event, void *context)
{
    (void)context;
    wifi_message_t message = {.kind = WIFI_MSG_EVENT};
    if (event != NULL) message.event = *event;
    if (xQueueSend(szpi_wifi_queue, &message, 0) != pdTRUE) {
        if (xSemaphoreTake(szpi_wifi_status_lock, 0) == pdTRUE) {
            szpi_wifi_status.event_queue_overflows++;
            xSemaphoreGive(szpi_wifi_status_lock);
        }
        // A queued diagnostic forces the service to reconcile state after overflow.
        uint32_t one = 1;
        (void)xQueueSend(szpi_supervisor_queue, &one, 0);
        xEventGroupSetBits(szpi_system_events, SZPI_EVENT_WIFI_OVERFLOW);
    }
    szpi_runtime_record_queue_peaks();
}

static void apply_event(const szpi_wifi_event_t *event)
{
    if (s_stopping) return;
    switch (event->id) {
    case SZPI_WIFI_EVENT_CONNECTED:
        set_state(SZPI_WIFI_LINK_UP, ESP_OK);
        break;
    case SZPI_WIFI_EVENT_GOT_IP:
        if (xSemaphoreTake(szpi_wifi_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            szpi_wifi_status.ip_info = event->ip_info;
            szpi_wifi_status.last_error = ESP_OK;
            szpi_wifi_status.attempts = 0;
            xSemaphoreGive(szpi_wifi_status_lock);
        }
        set_state(SZPI_WIFI_ONLINE, ESP_OK);
        start_time_sync();
        ESP_LOGI(TAG, "DHCP IPv4=" IPSTR " gateway=" IPSTR,
            IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
        break;
    case SZPI_WIFI_EVENT_DISCONNECTED:
        if (xSemaphoreTake(szpi_wifi_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            szpi_wifi_status.last_disconnect_reason = event->disconnect_reason;
            xSemaphoreGive(szpi_wifi_status_lock);
        }
        ESP_LOGW(TAG, "station disconnected reason=%d", event->disconnect_reason);
        set_state(SZPI_WIFI_RETRY_WAIT, ESP_FAIL);
        break;
    case SZPI_WIFI_EVENT_LOST_IP:
        set_state(SZPI_WIFI_RETRY_WAIT, ESP_FAIL);
        break;
    default:
        break;
    }
}

typedef enum { ROUND_ONLINE, ROUND_FAILED, ROUND_STOP, ROUND_RETRY } round_result_t;

static bool process_message(const wifi_message_t *message)
{
    if (message->kind == WIFI_MSG_EVENT) apply_event(&message->event);
    return message->kind == WIFI_MSG_STOP;
}

static round_result_t run_round(void)
{
    wifi_message_t pending;
    while (xQueueReceive(szpi_wifi_queue, &pending, 0) == pdTRUE) {
        if (pending.kind == WIFI_MSG_STOP) {
            s_stopping = true;
            return ROUND_STOP;
        }
        // Events and superseded commands from the prior round cannot promote
        // a new round to ONLINE. New events arrive after connect is issued.
    }
    int64_t round_start = esp_timer_get_time();
    if (xSemaphoreTake(szpi_wifi_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        szpi_wifi_status.attempts = 0;
        xSemaphoreGive(szpi_wifi_status_lock);
    }
    for (uint32_t attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        if (s_stopping) return ROUND_STOP;
        if ((esp_timer_get_time() - round_start) >= (int64_t)ROUND_TIMEOUT_MS * 1000) break;
        if (!s_adapter_initialized) {
            esp_err_t err = szpi_wifi_init(&s_config, szpi_wifi_post_event, NULL);
            if (err != ESP_OK) { set_state(SZPI_WIFI_FAILED, err); return ROUND_FAILED; }
            s_adapter_initialized = true;
        }
        if (!s_driver_started) {
            esp_err_t err = szpi_wifi_start();
            if (err != ESP_OK) { set_state(SZPI_WIFI_FAILED, err); return ROUND_FAILED; }
            s_driver_started = true;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (xSemaphoreTake(szpi_wifi_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            szpi_wifi_status.attempts++;
            xSemaphoreGive(szpi_wifi_status_lock);
        }
        set_state(SZPI_WIFI_CONNECTING, ESP_OK);
        esp_err_t err = szpi_wifi_connect();
        if (err != ESP_OK) {
            set_state(SZPI_WIFI_RETRY_WAIT, err);
        } else {
            int64_t attempt_deadline = esp_timer_get_time() + (int64_t)ATTEMPT_TIMEOUT_MS * 1000;
            while (!s_stopping && esp_timer_get_time() < attempt_deadline) {
                int64_t remain_ms = (attempt_deadline - esp_timer_get_time()) / 1000;
                wifi_message_t message;
                TickType_t wait = pdMS_TO_TICKS(remain_ms > 100 ? 100 : remain_ms);
                if (xQueueReceive(szpi_wifi_queue, &message, wait) == pdTRUE) {
                    if (process_message(&message)) { s_stopping = true; return ROUND_STOP; }
                    if (message.kind == WIFI_MSG_RETRY) return ROUND_RETRY;
                    szpi_wifi_status_t status;
                    if (szpi_app_wifi_get_status(&status) == ESP_OK && status.state == SZPI_WIFI_ONLINE) return ROUND_ONLINE;
                    if (message.kind == WIFI_MSG_EVENT && message.event.id == SZPI_WIFI_EVENT_DISCONNECTED) break;
                } else if ((xEventGroupGetBits(szpi_system_events) & SZPI_EVENT_WIFI_OVERFLOW) != 0) {
                    xEventGroupClearBits(szpi_system_events, SZPI_EVENT_WIFI_OVERFLOW);
                    set_state(SZPI_WIFI_RETRY_WAIT, ESP_ERR_TIMEOUT);
                    break;
                }
            }
        }
        if (s_stopping) return ROUND_STOP;
        (void)szpi_wifi_disconnect();
        if (attempt + 1U >= MAX_ATTEMPTS) break;
        int64_t delay_end = esp_timer_get_time() + (int64_t)s_retry_delay_s[attempt] * 1000000;
        set_state(SZPI_WIFI_RETRY_WAIT, ESP_OK);
        while (!s_stopping && esp_timer_get_time() < delay_end) {
            wifi_message_t message;
            TickType_t wait = pdMS_TO_TICKS(100);
            if (xQueueReceive(szpi_wifi_queue, &message, wait) == pdTRUE) {
                if (process_message(&message)) { s_stopping = true; return ROUND_STOP; }
                if (message.kind == WIFI_MSG_RETRY) return ROUND_RETRY;
                szpi_wifi_status_t status;
                if (szpi_app_wifi_get_status(&status) == ESP_OK && status.state == SZPI_WIFI_ONLINE) return ROUND_ONLINE;
            }
            if ((xEventGroupGetBits(szpi_system_events) & SZPI_EVENT_WIFI_OVERFLOW) != 0) {
                xEventGroupClearBits(szpi_system_events, SZPI_EVENT_WIFI_OVERFLOW);
                break;
            }
            if ((esp_timer_get_time() - round_start) >= (int64_t)ROUND_TIMEOUT_MS * 1000) break;
        }
    }
    set_state(SZPI_WIFI_FAILED, ESP_ERR_TIMEOUT);
    ESP_LOGW(TAG, "connection retry budget exhausted");
    return ROUND_FAILED;
}

static void stop_driver(void)
{
    s_stopping = true;
    set_state(SZPI_WIFI_STOPPING, ESP_OK);
    if (s_driver_started) {
        (void)szpi_wifi_disconnect();
        esp_err_t err = szpi_wifi_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi stop failed: %s", esp_err_to_name(err));
            set_state(SZPI_WIFI_FAILED, err);
            xEventGroupSetBits(szpi_system_events, SZPI_EVENT_WIFI_STOPPED);
            return;
        }
        s_driver_started = false;
    }
    set_state(SZPI_WIFI_STOPPED, ESP_OK);
    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_WIFI_STOPPED);
}

void szpi_wifi_service_task(void *context)
{
    (void)context;
    if (setenv("TZ", "CST-8", 1) == 0) tzset();
    else ESP_LOGW(TAG, "could not set local timezone; displaying UTC time");
    (void)xEventGroupWaitBits(szpi_system_events, SZPI_EVENT_RUNTIME_START, pdFALSE, pdTRUE, portMAX_DELAY);
    for (;;) {
        wifi_message_t message;
        if (xQueueReceive(szpi_wifi_queue, &message, portMAX_DELAY) != pdTRUE) continue;
        if (message.kind == WIFI_MSG_STOP) { stop_driver(); continue; }
        if (message.kind != WIFI_MSG_START && message.kind != WIFI_MSG_RETRY) {
            if (message.kind == WIFI_MSG_EVENT) apply_event(&message.event);
            continue;
        }
        szpi_wifi_status_t current;
        if (szpi_app_wifi_get_status(&current) == ESP_OK &&
            current.state != SZPI_WIFI_STOPPED && current.state != SZPI_WIFI_FAILED) continue;
        s_stopping = false;
        if (message.kind == WIFI_MSG_START && !s_adapter_initialized) {
            set_state(SZPI_WIFI_CONNECTING, ESP_OK);
        }
        round_result_t result;
        do { result = run_round(); } while (result == ROUND_RETRY && !s_stopping);
        if (result == ROUND_STOP || s_stopping) { stop_driver(); continue; }
        while (result == ROUND_ONLINE && !s_stopping) {
            if (xQueueReceive(szpi_wifi_queue, &message, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (message.kind == WIFI_MSG_STOP) { s_stopping = true; break; }
                if (message.kind == WIFI_MSG_RETRY) {
                    (void)szpi_wifi_disconnect();
                    set_state(SZPI_WIFI_CONNECTING, ESP_OK);
                    result = ROUND_RETRY;
                    break;
                }
                if (message.kind == WIFI_MSG_EVENT) {
                    apply_event(&message.event);
                    if (message.event.id == SZPI_WIFI_EVENT_DISCONNECTED || message.event.id == SZPI_WIFI_EVENT_LOST_IP) result = ROUND_RETRY;
                }
            } else if ((xEventGroupGetBits(szpi_system_events) & SZPI_EVENT_WIFI_OVERFLOW) != 0) {
                xEventGroupClearBits(szpi_system_events, SZPI_EVENT_WIFI_OVERFLOW);
                set_state(SZPI_WIFI_RETRY_WAIT, ESP_ERR_TIMEOUT);
                result = ROUND_RETRY;
            }
            if (result == ROUND_RETRY) {
                result = run_round();
                if (result == ROUND_STOP) break;
            }
        }
        if (s_stopping || result == ROUND_STOP) { stop_driver(); continue; }
    }
}
