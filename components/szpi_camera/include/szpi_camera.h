#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define SZPI_CAMERA_WIDTH 320U
#define SZPI_CAMERA_HEIGHT 240U
#define SZPI_CAMERA_FRAME_BYTES (SZPI_CAMERA_WIDTH * SZPI_CAMERA_HEIGHT * 2U)

typedef struct {
    const uint8_t *data;
    size_t length;
    uint16_t width;
    uint16_t height;
    uint32_t token;
    void *driver_frame;
} szpi_camera_frame_t;

// init/acquire/release/deinit are serialized and task-context only. Acquire
// may block for up to the vendor driver's fixed four-second timeout. Frames
// must be released by the task that acquired them; deinit refuses outstanding
// frames and an acquire in progress.
esp_err_t szpi_camera_init(uint16_t *detected_pid);
esp_err_t szpi_camera_acquire(szpi_camera_frame_t *frame);
esp_err_t szpi_camera_release(szpi_camera_frame_t *frame);
esp_err_t szpi_camera_deinit(void);
