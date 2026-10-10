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
    SZPI_UI_EVENT_SPEAKER_VOLUME_CHANGED,
    SZPI_UI_EVENT_SPEAKER_VOLUME_SAVE,
    SZPI_UI_EVENT_MIC_GAIN_CHANGED,
    SZPI_UI_EVENT_MIC_GAIN_SAVE,
    SZPI_UI_EVENT_AUDIO_TEST_TONE,
    SZPI_UI_EVENT_AUDIO_TEST_CAPTURE,
    SZPI_UI_EVENT_AUDIO_STOP,
    SZPI_UI_EVENT_OTA_START,
    SZPI_UI_EVENT_STORAGE_RETRY,
    SZPI_UI_EVENT_STORAGE_FORMAT,
    SZPI_UI_EVENT_CAMERA_TEST_START,
    SZPI_UI_EVENT_CAMERA_TEST_STOP,
} szpi_ui_event_t;

typedef enum {
    SZPI_UI_CAMERA_UNAVAILABLE,
    SZPI_UI_CAMERA_STOPPED,
    SZPI_UI_CAMERA_STARTING,
    SZPI_UI_CAMERA_RUNNING,
    SZPI_UI_CAMERA_STOPPING,
    SZPI_UI_CAMERA_FAULT,
} szpi_ui_camera_state_t;

typedef enum {
    SZPI_UI_AUDIO_OFFLINE,
    SZPI_UI_AUDIO_IDLE,
    SZPI_UI_AUDIO_PLAYING_TEST,
    SZPI_UI_AUDIO_CAPTURE_TEST,
    SZPI_UI_AUDIO_PLAYING_CAPTURE_TEST,
    SZPI_UI_AUDIO_RECORDING,
    SZPI_UI_AUDIO_PLAYING_FILE,
    SZPI_UI_AUDIO_COMPLETE,
    SZPI_UI_AUDIO_FAULT,
} szpi_ui_audio_state_t;

typedef enum {
    SZPI_UI_STORAGE_UNINITIALIZED,
    SZPI_UI_STORAGE_NO_CARD,
    SZPI_UI_STORAGE_CARD_READY_NO_FS,
    SZPI_UI_STORAGE_READY,
    SZPI_UI_STORAGE_BUSY,
    SZPI_UI_STORAGE_FORMATTING,
    SZPI_UI_STORAGE_FAULT,
} szpi_ui_storage_state_t;

typedef enum {
    SZPI_UI_OTA_IDLE,
    SZPI_UI_OTA_REQUESTED,
    SZPI_UI_OTA_CONNECTING,
    SZPI_UI_OTA_DOWNLOADING,
    SZPI_UI_OTA_RESTARTING,
    SZPI_UI_OTA_FAILED,
} szpi_ui_ota_state_t;

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
    uint8_t speaker_volume_percent;
    uint8_t microphone_gain_percent;
    szpi_ui_audio_state_t audio_state;
    uint32_t audio_blocks_processed;
    uint16_t audio_peak_sample;
    uint16_t audio_rms_sample;
    uint32_t audio_error_code;
    szpi_ui_storage_state_t storage_state;
    uint64_t storage_capacity_bytes;
    uint64_t storage_free_bytes;
    uint32_t storage_generation;
    uint32_t storage_max_frequency_khz;
    uint8_t storage_fat_type;
    uint32_t storage_error_code;
    uint8_t display_brightness_percent;
    bool display_inverted;
    bool display_test_supported;
    bool display_test_valid;
    char firmware_version[32];
    char running_ota_slot[17];
    char hostname[33];
    uint32_t display_fps_x10;
    uint32_t display_frame_avg_us;
    uint32_t display_frame_max_us;
    uint32_t display_lvgl_avg_us;
    uint32_t display_gap_avg_us;
    szpi_ui_ota_state_t ota_state;
    uint8_t ota_progress_percent;
    uint32_t ota_error_code;
    szpi_ui_camera_state_t camera_state;
    uint32_t camera_fps_milli;
    uint32_t camera_error_count;
    uint32_t camera_error_code;
} szpi_ui_model_t;

typedef void (*szpi_ui_event_cb_t)(szpi_ui_event_t event, uint32_t value, void *context);

/*
 * All functions must be called from the thread that owns LVGL.
 * create() requires lv_init() and a registered display, and returns INVALID_STATE
 * if called before destroy(). update() copies the supplied state. destroy() is
 * idempotent and deletes the UI objects, not the LVGL display. Event callbacks
 * run inline on the LVGL thread and must not perform blocking work.
 * Brightness, speaker volume, and microphone gain change/save events carry
 * percentages (0..100). Storage format carries the current card generation;
 * storage retry and test start/stop events carry zero. Network cancel/forget
 * carry a local page request identity (cancel) or model generation (forget).
 * Network begin encodes (request identity << 1) | DPP flag.
 */
szpi_ui_result_t szpi_ui_create(szpi_ui_event_cb_t event_cb, void *context);
szpi_ui_result_t szpi_ui_update(const szpi_ui_model_t *model);
void szpi_ui_destroy(void);

/* UI-thread only. Source is a 320x240 native-endian RGB565 staging buffer.
 * The caller owns source and pixels, keeps both valid while bound, and waits
 * for rendering / DMA before modifying them. NULL unbinds the image. */
szpi_ui_result_t szpi_ui_camera_test_set_frame(const lv_image_dsc_t *source);

#endif
