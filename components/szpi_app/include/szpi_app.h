#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "szpi_wifi.h"

typedef enum {
    SZPI_WIFI_DISABLED,
    SZPI_WIFI_NO_CONFIG,
    SZPI_WIFI_STOPPED,
    SZPI_WIFI_CONNECTING,
    SZPI_WIFI_LINK_UP,
    SZPI_WIFI_ONLINE,
    SZPI_WIFI_RETRY_WAIT,
    SZPI_WIFI_FAILED,
    SZPI_WIFI_STOPPING,
} szpi_wifi_service_state_t;

typedef struct {
    szpi_wifi_service_state_t state;
    esp_netif_ip_info_t ip_info;
    int8_t rssi;
    bool rssi_valid;
    int last_disconnect_reason;
    uint32_t attempts;
    esp_err_t last_error;
    uint32_t event_queue_overflows;
} szpi_wifi_status_t;

typedef enum {
    SZPI_UI_STOPPED,
    SZPI_UI_STARTING,
    SZPI_UI_READY,
    SZPI_UI_TOUCH_FAULT,
    SZPI_UI_DISPLAY_FAULT,
    SZPI_UI_STOPPING,
    SZPI_UI_FAILED,
} szpi_ui_state_t;

typedef struct {
    szpi_ui_state_t state;
    uint16_t touch_x;
    uint16_t touch_y;
    uint8_t brightness_percent;
    bool touch_pressed;
    bool touch_faulted;
    uint32_t click_count;
    uint32_t touch_errors;
    uint32_t touch_max_read_duration_us;
    uint32_t flush_timeouts;
    esp_err_t last_error;
} szpi_ui_status_t;

// Call once after NVS and board initialization. Creates all enabled project tasks.
esp_err_t szpi_app_runtime_start(const szpi_wifi_config_t *wifi_config, bool wifi_enabled, bool ui_enabled);
// Starts only diagnostics after a core boot failure; never publishes SYSTEM_READY.
esp_err_t szpi_app_runtime_fault(esp_err_t cause);
// start/retry enqueue commands and return once accepted; both are task-context APIs.
esp_err_t szpi_app_wifi_start(void);
// Sends cooperative STOP and waits for driver stop confirmation up to timeout_ticks.
esp_err_t szpi_app_wifi_stop(TickType_t timeout_ticks);
esp_err_t szpi_app_wifi_retry(void);
// Copies a consistent snapshot; RSSI is queried from the Wi-Fi driver when online.
esp_err_t szpi_app_wifi_get_status(szpi_wifi_status_t *status);
esp_err_t szpi_app_ui_start(void);
esp_err_t szpi_app_ui_stop(TickType_t timeout_ticks);
esp_err_t szpi_app_ui_get_status(szpi_ui_status_t *status);
EventGroupHandle_t szpi_app_get_system_events(void);
