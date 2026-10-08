#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "szpi_wifi.h"
#include "szpi_camera.h"
#include "szpi_storage.h"

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
    szpi_wifi_link_info_t link_info;
    int last_disconnect_reason;
    uint32_t attempts;
    esp_err_t last_error;
    uint32_t event_queue_overflows;
    uint32_t sys_evt_stack_min_bytes;
    bool has_config;
    char ssid[33];
    szpi_provision_status_t provisioning;
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

typedef enum {
    SZPI_CAMERA_PREVIEW_UNAVAILABLE,
    SZPI_CAMERA_PREVIEW_STOPPED,
    SZPI_CAMERA_PREVIEW_STARTING,
    SZPI_CAMERA_PREVIEW_RUNNING,
    SZPI_CAMERA_PREVIEW_STOPPING,
    SZPI_CAMERA_PREVIEW_FAULT,
} szpi_camera_preview_state_t;

typedef struct {
    szpi_camera_preview_state_t state;
    uint16_t sensor_pid;
    bool frame_outstanding;
    uint32_t captured_frames;
    uint32_t displayed_frames;
    uint32_t dropped_frames;
    uint32_t error_count;
    uint32_t capture_fps_milli;
    uint32_t display_fps_milli;
    uint32_t max_capture_us;
    uint32_t max_copy_us;
    uint32_t max_refresh_us;
    esp_err_t last_error;
} szpi_camera_preview_status_t;

typedef enum {
    SZPI_AUDIO_SERVICE_OFFLINE,
    SZPI_AUDIO_SERVICE_IDLE,
    SZPI_AUDIO_SERVICE_PLAYING_TEST,
    SZPI_AUDIO_SERVICE_CAPTURE_TEST,
    SZPI_AUDIO_SERVICE_PLAYING_CAPTURE_TEST,
    SZPI_AUDIO_SERVICE_RECORDING,
    SZPI_AUDIO_SERVICE_PLAYING_FILE,
    SZPI_AUDIO_SERVICE_COMPLETE,
    SZPI_AUDIO_SERVICE_FAULT,
} szpi_audio_service_state_t;

typedef struct {
    szpi_audio_service_state_t state;
    uint32_t blocks_processed;
    uint16_t peak_sample;
    uint16_t rms_sample;
    uint8_t input_gain_db;
    uint8_t input_gain_percent;
    uint8_t output_volume_percent;
    uint32_t errors;
    esp_err_t last_error;
    char last_file[40];
} szpi_audio_service_status_t;

// Call once after NVS and board initialization. Creates all enabled project tasks.
esp_err_t szpi_app_runtime_start(bool wifi_enabled, bool ui_enabled);
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
esp_err_t szpi_app_camera_preview_start(void);
esp_err_t szpi_app_camera_preview_request_stop(void);
esp_err_t szpi_app_camera_preview_stop(TickType_t timeout_ticks);
esp_err_t szpi_app_camera_preview_get_status(szpi_camera_preview_status_t *status);
esp_err_t szpi_app_storage_get_status(szpi_storage_status_t *status);
// Formatting is queued to the runtime storage task. Token must come from a status
// snapshot taken during the immediately preceding explicit UI confirmation.
esp_err_t szpi_app_storage_format_confirmed(uint32_t generation);
esp_err_t szpi_app_storage_retry(void);
esp_err_t szpi_app_audio_test_tone(void);
esp_err_t szpi_app_audio_capture_test(void);
esp_err_t szpi_app_audio_record_start(void);
esp_err_t szpi_app_audio_play_latest(void);
esp_err_t szpi_app_audio_set_volume(uint8_t volume_percent);
esp_err_t szpi_app_audio_set_input_gain(uint8_t gain_db);
esp_err_t szpi_app_audio_set_input_gain_percent(uint8_t gain_percent);
esp_err_t szpi_app_audio_save_settings(void);
esp_err_t szpi_app_audio_stop(void);
esp_err_t szpi_app_audio_get_status(szpi_audio_service_status_t *status);
EventGroupHandle_t szpi_app_get_system_events(void);

/* Nonblocking local GUI commands; candidate testing and NVS mutation run only
 * on the runtime Wi-Fi owner task. Begin/cancel use a GUI request identity;
 * forget uses the visible generation. Stale page exits cannot stop a later session. */
esp_err_t szpi_app_wifi_provision_begin(bool use_dpp, uint32_t request_id);
esp_err_t szpi_app_wifi_provision_cancel(uint32_t request_id);
esp_err_t szpi_app_wifi_forget(uint32_t generation);
