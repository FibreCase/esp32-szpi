#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"

#define SZPI_DISPLAY_WIDTH 320
#define SZPI_DISPLAY_HEIGHT 240
#define SZPI_DISPLAY_BUFFER_LINES 40
#define SZPI_DISPLAY_BUFFER_BYTES (SZPI_DISPLAY_WIDTH * SZPI_DISPLAY_BUFFER_LINES * 2)

// Requires lv_init() from the UI task. Owns the LVGL display binding and DMA buffers.
// The PCA9557 LCD_CS stays selected until deinit.
esp_err_t szpi_display_init(lv_display_t **display);
// Waits for the one in-flight SPI DMA flush. Timeout never releases its buffer.
esp_err_t szpi_display_wait_flush(TickType_t timeout_ticks);
esp_err_t szpi_display_get_last_error(void);
uint32_t szpi_display_get_flush_timeout_count(void);
typedef struct {
    bool valid;
    uint32_t fps_x10;
    uint32_t frame_avg_us;
    uint32_t frame_max_us;
    uint32_t lvgl_avg_us;
    uint32_t gap_avg_us;
} szpi_display_test_stats_t;

// UI-task only. Enable resets the sample window after draining any active DMA.
// Disable stops sampling without changing display timing or buffer ownership.
esp_err_t szpi_display_set_test_active(bool active);
// UI-task only. Copies the latest one-second sample; never waits for DMA.
esp_err_t szpi_display_get_test_stats(szpi_display_test_stats_t *stats);
// UI-task only; waits for any in-flight DMA before changing the panel's 180-degree orientation.
esp_err_t szpi_display_set_orientation_inverted(bool inverted);
esp_err_t szpi_display_set_brightness(uint8_t percent);
esp_err_t szpi_display_deinit(void);
