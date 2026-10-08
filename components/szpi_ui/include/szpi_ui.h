#ifndef SZPI_UI_H
#define SZPI_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef enum {
    SZPI_UI_EVENT_PRIMARY_ACTION = 1,
    SZPI_UI_EVENT_DISPLAY_TEST_START,
    SZPI_UI_EVENT_DISPLAY_TEST_STOP,
    SZPI_UI_EVENT_BRIGHTNESS_CHANGED,
    SZPI_UI_EVENT_BRIGHTNESS_SAVE,
    SZPI_UI_EVENT_NETWORK_BEGIN,
    SZPI_UI_EVENT_NETWORK_CANCEL,
    SZPI_UI_EVENT_NETWORK_FORGET,
} szpi_ui_event_t;

typedef enum {
    SZPI_UI_RESULT_OK = 0,
    SZPI_UI_RESULT_INVALID_ARGUMENT,
    SZPI_UI_RESULT_INVALID_STATE,
    SZPI_UI_RESULT_NO_MEMORY,
} szpi_ui_result_t;

typedef struct {
    uint32_t click_count;
    uint32_t imu_sequence;
    float accel_g[3];
    bool imu_available;
    bool imu_valid;
    bool network_connected;
    bool network_supported;
    bool network_needs_setup;
    bool network_has_config;
    bool provisioning_active;
    bool dpp_ready;
    uint8_t provisioning_state;
    uint32_t provisioning_generation;
    char network_ssid[33];
    bool network_details_valid;
    int8_t network_rssi;
    uint8_t network_channel;
    char network_ip[16];
    char network_gateway[16];
    char network_netmask[16];
    char network_mac[18];
    char network_bssid[18];
    char provisioning_message[96];
    char setup_ssid[33];
    char setup_password[17];
    char setup_wifi_qr[160];
    char setup_dpp_uri[512];
    bool time_valid;
    char time_text[6];
    uint8_t display_brightness_percent;
    bool display_inverted;
    bool display_test_supported;
    bool display_test_valid;
    uint32_t display_fps_x10;
    uint32_t display_frame_avg_us;
    uint32_t display_frame_max_us;
    uint32_t display_lvgl_avg_us;
    uint32_t display_gap_avg_us;
} szpi_ui_model_t;

typedef void (*szpi_ui_event_cb_t)(szpi_ui_event_t event, uint32_t value, void *context);

/*
 * All functions must be called from the thread that owns LVGL.
 * create() requires lv_init() and a registered display, and returns INVALID_STATE
 * if called before destroy(). update() copies the supplied state. destroy() is
 * idempotent and deletes the UI objects, not the LVGL display. Event callbacks
 * run inline on the LVGL thread and must not perform blocking work.
 * Brightness events carry a percentage (0..100); network cancel/forget carry
 * a local page request identity (cancel) or model generation (forget). Network
 * begin encodes (request identity << 1) | DPP flag.
 * Other events carry zero.
 */
szpi_ui_result_t szpi_ui_create(szpi_ui_event_cb_t event_cb, void *context);
szpi_ui_result_t szpi_ui_update(const szpi_ui_model_t *model);
void szpi_ui_destroy(void);

#endif
