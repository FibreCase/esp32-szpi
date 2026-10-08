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
// UI-task only; waits for any in-flight DMA before changing the panel's 180-degree orientation.
esp_err_t szpi_display_set_orientation_inverted(bool inverted);
esp_err_t szpi_display_set_brightness(uint8_t percent);
esp_err_t szpi_display_deinit(void);
