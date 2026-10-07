#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "szpi_board.h"
#include "szpi_camera.h"

#define TAG "szpi_camera"
#define CAMERA_STARTUP_DELAY_MS 10U

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static const szpi_board_bindings_t *s_bindings;
static camera_fb_t *s_outstanding_frame;
static TaskHandle_t s_frame_owner;
static uint32_t s_next_token;
static uint32_t s_outstanding_token;
static bool s_initialized;
static bool s_acquire_in_progress;

static void make_camera_config(camera_config_t *config)
{
    *config = (camera_config_t){
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = s_bindings->camera_xclk,
        .pin_sccb_sda = -1,
        .pin_sccb_scl = -1,
        .pin_d0 = s_bindings->camera_data[0],
        .pin_d1 = s_bindings->camera_data[1],
        .pin_d2 = s_bindings->camera_data[2],
        .pin_d3 = s_bindings->camera_data[3],
        .pin_d4 = s_bindings->camera_data[4],
        .pin_d5 = s_bindings->camera_data[5],
        .pin_d6 = s_bindings->camera_data[6],
        .pin_d7 = s_bindings->camera_data[7],
        .pin_vsync = s_bindings->camera_vsync,
        .pin_href = s_bindings->camera_href,
        .pin_pclk = s_bindings->camera_pclk,
        .xclk_freq_hz = s_bindings->camera_xclk_frequency_hz,
        .ledc_timer = s_bindings->camera_xclk_timer,
        .ledc_channel = s_bindings->camera_xclk_channel,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .sccb_i2c_port = s_bindings->i2c_port,
        .jpeg_buffer_size = 0,
    };
}

esp_err_t szpi_camera_init(uint16_t *detected_pid)
{
    if (detected_pid == NULL) return ESP_ERR_INVALID_ARG;
    *detected_pid = 0;
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_initialized || s_acquire_in_progress || s_outstanding_frame != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_handle_t shared_bus;
    esp_err_t err = szpi_board_get_bindings(&s_bindings);
    if (err == ESP_OK) err = szpi_board_get_i2c_bus(&shared_bus);
    if (err != ESP_OK) goto done;
    (void)shared_bus;

    err = szpi_board_set_camera_powered(true, pdMS_TO_TICKS(100));
    if (err != ESP_OK) goto done;
    vTaskDelay(pdMS_TO_TICKS(CAMERA_STARTUP_DELAY_MS));

    camera_config_t config;
    make_camera_config(&config);
    err = esp_camera_init(&config);
    if (err != ESP_OK) {
        (void)szpi_board_set_camera_powered(false, pdMS_TO_TICKS(100));
        goto done;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    camera_sensor_info_t *sensor_info = sensor == NULL ? NULL : esp_camera_sensor_get_info(&sensor->id);
    if (sensor == NULL || sensor->id.PID != s_bindings->camera_pid || sensor_info == NULL ||
        sensor_info->sccb_addr != s_bindings->camera_sccb_address) {
        err = ESP_ERR_NOT_SUPPORTED;
        (void)esp_camera_deinit();
        (void)szpi_board_set_camera_powered(false, pdMS_TO_TICKS(100));
        goto done;
    }
    s_initialized = true;
    *detected_pid = sensor->id.PID;
    ESP_LOGI(TAG, "GC2145 PID=0x%04x SCCB=0x%02x QVGA RGB565 XCLK=%" PRIu32 "Hz fb=%u PSRAM DMA=%s",
        (unsigned)*detected_pid, (unsigned)sensor_info->sccb_addr,
        s_bindings->camera_xclk_frequency_hz, 1U,
        esp_camera_get_psram_mode() ? "enabled" : "disabled");

done:
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t szpi_camera_acquire(szpi_camera_frame_t *frame)
{
    if (frame == NULL) return ESP_ERR_INVALID_ARG;
    memset(frame, 0, sizeof(*frame));
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_initialized || s_outstanding_frame != NULL || s_acquire_in_progress) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_acquire_in_progress = true;
    xSemaphoreGive(s_lock);

    int64_t started = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - started);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (fb != NULL) esp_camera_fb_return(fb);
        return ESP_ERR_TIMEOUT;
    }
    s_acquire_in_progress = false;
    if (fb == NULL) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "frame acquire failed after %" PRIu32 " us", elapsed_us);
        return ESP_ERR_TIMEOUT;
    }
    size_t width = fb->width;
    size_t height = fb->height;
    size_t length = fb->len;
    pixformat_t format = fb->format;
    bool in_psram = fb->buf != NULL && esp_ptr_external_ram(fb->buf);
    if (fb->buf == NULL || width != SZPI_CAMERA_WIDTH || height != SZPI_CAMERA_HEIGHT ||
        length != SZPI_CAMERA_FRAME_BYTES || format != PIXFORMAT_RGB565 || !in_psram) {
        esp_camera_fb_return(fb);
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "invalid frame: %zux%zu len=%zu fmt=%d psram=%d", width, height,
            length, (int)format, (int)in_psram);
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_outstanding_frame = fb;
    s_frame_owner = xTaskGetCurrentTaskHandle();
    s_outstanding_token = ++s_next_token;
    if (s_outstanding_token == 0) s_outstanding_token = ++s_next_token;
    *frame = (szpi_camera_frame_t){
        .data = fb->buf,
        .length = fb->len,
        .width = (uint16_t)fb->width,
        .height = (uint16_t)fb->height,
        .token = s_outstanding_token,
        .driver_frame = fb,
    };
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t szpi_camera_release(szpi_camera_frame_t *frame)
{
    if (frame == NULL || frame->driver_frame == NULL || s_lock == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_initialized || s_outstanding_frame != frame->driver_frame ||
        frame->token != s_outstanding_token || s_frame_owner != xTaskGetCurrentTaskHandle()) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_camera_fb_return(s_outstanding_frame);
    s_outstanding_frame = NULL;
    s_frame_owner = NULL;
    s_outstanding_token = 0;
    memset(frame, 0, sizeof(*frame));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t szpi_camera_deinit(void)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_initialized || s_outstanding_frame != NULL || s_acquire_in_progress) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_camera_deinit();
    if (err == ESP_OK) {
        s_initialized = false;
        err = szpi_board_set_camera_powered(false, pdMS_TO_TICKS(100));
    }
    s_bindings = NULL;
    xSemaphoreGive(s_lock);
    return err;
}
