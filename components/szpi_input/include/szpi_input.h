#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t touch_count;
    uint8_t primary_id;
    bool pressed;
    bool faulted;
    uint32_t consecutive_errors;
    uint32_t read_duration_us;
} szpi_input_state_t;

esp_err_t szpi_input_init(void);
// Reads and maps one stable primary point. No touch returns a released state.
esp_err_t szpi_input_read(szpi_input_state_t *state);
esp_err_t szpi_input_deinit(void);

typedef struct {
    uint8_t who_am_i;
    uint8_t revision;
} szpi_imu_info_t;

typedef struct {
    int16_t accel_raw[3];
    int16_t gyro_raw[3];
    float accel_g[3];
    float gyro_dps[3];
    float roll_deg;
    float pitch_deg;
    uint32_t sequence;
    uint32_t read_duration_us;
    uint32_t consecutive_errors;
    int64_t timestamp_us;
    esp_err_t last_error;
    bool valid;
    bool fresh;
    bool tilt_reliable;
    bool faulted;
} szpi_imu_sample_t;

// IMU and BOOT APIs are task-context only and owned by the sole szpi_ui task.
// IMU transactions use the board-owned I2C bus, with a 5 ms per-transaction
// timeout. Reads return ESP_OK with fresh=false when no paired sample is ready.
esp_err_t szpi_imu_init(szpi_imu_info_t *info);
esp_err_t szpi_imu_read(szpi_imu_sample_t *sample);
esp_err_t szpi_imu_deinit(void);

typedef enum {
    SZPI_BOOT_EVENT_NONE = 0,
    SZPI_BOOT_EVENT_PRESSED = 1U << 0,
    SZPI_BOOT_EVENT_LONG_PRESS = 1U << 1,
    SZPI_BOOT_EVENT_RELEASED = 1U << 2,
    SZPI_BOOT_EVENT_SHORT_PRESS = 1U << 3,
} szpi_boot_event_t;

typedef struct {
    szpi_boot_event_t events;
    uint64_t timestamp_us;
    bool pressed;
    bool faulted;
} szpi_boot_state_t;

esp_err_t szpi_boot_button_init(void);
// Events are edge/threshold events for this poll only and are not repeated.
esp_err_t szpi_boot_button_read(szpi_boot_state_t *state);
esp_err_t szpi_boot_button_deinit(void);
