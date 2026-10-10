#include <inttypes.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "szpi_app.h"
#include "szpi_camera.h"
#include "szpi_runtime_internal.h"

#define TAG "szpi_preview"
#define PREVIEW_CMD_START (1U << 0)
#define PREVIEW_CMD_STOP (1U << 1)
#define PREVIEW_FRAME_PERIOD_MS 67U
#define PREVIEW_ACK_WAIT_MS 100U
#define PREVIEW_ACK_LIMIT_MS 5000U
#define PREVIEW_FAILURE_LIMIT 3U

static bool s_driver_initialized;
static bool s_stop_latched;
static uint32_t s_generation;

static void set_preview_state(szpi_camera_preview_state_t state, esp_err_t error)
{
    if (xSemaphoreTake(szpi_preview_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    szpi_preview_status.state = state;
    szpi_preview_status.last_error = error;
    if (error != ESP_OK) szpi_preview_status.error_count++;
    xSemaphoreGive(szpi_preview_status_lock);
}

static void increment_counter(uint32_t *field)
{
    if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
        (*field)++;
        xSemaphoreGive(szpi_preview_status_lock);
    }
}

static void set_maximum(uint32_t *field, uint32_t value)
{
    if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
        if (value > *field) *field = value;
        xSemaphoreGive(szpi_preview_status_lock);
    }
}

static bool stop_requested(void)
{
    if (s_stop_latched) return true;
    uint32_t command = 0;
    (void)xTaskNotifyWait(0, UINT32_MAX, &command, 0);
    s_stop_latched = (command & PREVIEW_CMD_STOP) != 0;
    return s_stop_latched;
}

static void set_terminal_state(szpi_camera_preview_state_t state, esp_err_t error)
{
    set_preview_state(state, error);
    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_PREVIEW_STOPPED);
}

static esp_err_t stop_camera(void)
{
    if (!s_driver_initialized) return ESP_OK;
    esp_err_t err = szpi_camera_deinit();
    if (err == ESP_OK) s_driver_initialized = false;
    return err;
}

static bool wait_for_frame_ack(const szpi_preview_frame_message_t *message)
{
    const TickType_t started = xTaskGetTickCount();
    szpi_preview_ack_message_t ack;
    bool stop_logged = false;
    for (;;) {
        if (xQueueReceive(szpi_preview_ack_queue, &ack, pdMS_TO_TICKS(PREVIEW_ACK_WAIT_MS)) == pdTRUE) {
            if (ack.generation == message->generation && ack.token == message->frame.token) return true;
            ESP_LOGW(TAG, "discarding stale frame ack token=%" PRIu32 " generation=%" PRIu32,
                ack.token, ack.generation);
        }
        if ((xTaskGetTickCount() - started) >= pdMS_TO_TICKS(PREVIEW_ACK_LIMIT_MS)) return false;
        if (stop_requested() && !stop_logged) {
            // The frame remains owned until UI confirms its staging copy.
            ESP_LOGI(TAG, "stop requested while waiting for UI copy acknowledgement");
            stop_logged = true;
        }
    }
}

static void run_preview(void)
{
    set_preview_state(SZPI_CAMERA_PREVIEW_STARTING, ESP_OK);
    xQueueReset(szpi_preview_frame_queue);
    xQueueReset(szpi_preview_ack_queue);
    s_generation++;
    if (s_generation == 0) s_generation++;

    uint16_t pid = 0;
    esp_err_t err = szpi_camera_init(&pid);
    if (err != ESP_OK) {
        set_terminal_state(SZPI_CAMERA_PREVIEW_UNAVAILABLE, err);
        ESP_LOGE(TAG, "camera initialization failed: %s", esp_err_to_name(err));
        return;
    }
    s_driver_initialized = true;
    if (xSemaphoreTake(szpi_preview_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        szpi_preview_status.sensor_pid = pid;
        szpi_preview_status.frame_outstanding = false;
        szpi_preview_status.captured_frames = 0;
        szpi_preview_status.displayed_frames = 0;
        szpi_preview_status.dropped_frames = 0;
        szpi_preview_status.error_count = 0;
        szpi_preview_status.capture_fps_milli = 0;
        szpi_preview_status.display_fps_milli = 0;
        szpi_preview_status.max_capture_us = 0;
        szpi_preview_status.max_copy_us = 0;
        szpi_preview_status.max_refresh_us = 0;
        xSemaphoreGive(szpi_preview_status_lock);
    }
    set_preview_state(SZPI_CAMERA_PREVIEW_RUNNING, ESP_OK);
    ESP_LOGI(TAG, "preview running generation=%" PRIu32, s_generation);

    uint32_t consecutive_errors = 0;
    bool capture_failed = false;
    esp_err_t terminal_error = ESP_OK;
    uint32_t period_frames = 0;
    int64_t period_started_us = esp_timer_get_time();
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        if (stop_requested()) break;
        szpi_camera_frame_t frame = {0};
        int64_t capture_started_us = esp_timer_get_time();
        err = szpi_camera_acquire(&frame);
        uint32_t capture_us = (uint32_t)(esp_timer_get_time() - capture_started_us);
        set_maximum(&szpi_preview_status.max_capture_us, capture_us);
        if (err != ESP_OK) {
            consecutive_errors++;
            increment_counter(&szpi_preview_status.error_count);
            if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
                szpi_preview_status.last_error = err;
                xSemaphoreGive(szpi_preview_status_lock);
            }
            ESP_LOGW(TAG, "capture error %" PRIu32 "/%u: %s", consecutive_errors,
                PREVIEW_FAILURE_LIMIT, esp_err_to_name(err));
            if (consecutive_errors >= PREVIEW_FAILURE_LIMIT) {
                capture_failed = true;
                terminal_error = err;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        consecutive_errors = 0;
        if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
            szpi_preview_status.frame_outstanding = true;
            xSemaphoreGive(szpi_preview_status_lock);
        }
        increment_counter(&szpi_preview_status.captured_frames);
        period_frames++;
        int64_t now_us = esp_timer_get_time();
        if (now_us - period_started_us >= 1000000) {
            uint32_t fps_milli = (uint32_t)(((uint64_t)period_frames * 1000000000ULL) /
                (uint64_t)(now_us - period_started_us));
            if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
                szpi_preview_status.capture_fps_milli = fps_milli;
                xSemaphoreGive(szpi_preview_status_lock);
            }
            period_frames = 0;
            period_started_us = now_us;
        }

        szpi_preview_frame_message_t message = {.frame = frame, .generation = s_generation};
        if (xQueueSend(szpi_preview_frame_queue, &message, 0) != pdTRUE) {
            increment_counter(&szpi_preview_status.dropped_frames);
            err = szpi_camera_release(&frame);
            if (err == ESP_OK) {
                if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
                    szpi_preview_status.frame_outstanding = false;
                    xSemaphoreGive(szpi_preview_status_lock);
                }
            } else {
                set_preview_state(SZPI_CAMERA_PREVIEW_FAULT, err);
                for (;;) vTaskDelay(portMAX_DELAY);
            }
        } else if (wait_for_frame_ack(&message)) {
            // The UI has finished copying into its owned staging buffer.
            err = szpi_camera_release(&frame);
            if (err != ESP_OK) {
                set_preview_state(SZPI_CAMERA_PREVIEW_FAULT, err);
                for (;;) vTaskDelay(portMAX_DELAY);
            }
            if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
                szpi_preview_status.frame_outstanding = false;
                xSemaphoreGive(szpi_preview_status_lock);
            }
        } else {
            // Keep the driver and borrowed frame alive. A missing UI ack means
            // it may still be reading the source, so no safe deinit is possible.
            set_preview_state(SZPI_CAMERA_PREVIEW_FAULT, ESP_ERR_TIMEOUT);
            ESP_LOGE(TAG, "UI copy acknowledgement timed out; retaining frame and camera resources");
            for (;;) vTaskDelay(portMAX_DELAY);
        }
        if (stop_requested()) break;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PREVIEW_FRAME_PERIOD_MS));
    }

    set_preview_state(SZPI_CAMERA_PREVIEW_STOPPING, ESP_OK);
    err = stop_camera();
    if (err == ESP_OK && !capture_failed) set_terminal_state(SZPI_CAMERA_PREVIEW_STOPPED, ESP_OK);
    else set_terminal_state(SZPI_CAMERA_PREVIEW_FAULT, err != ESP_OK ? err : terminal_error);
}

void szpi_camera_preview_service_task(void *context)
{
    (void)context;
    (void)xEventGroupWaitBits(szpi_system_events, SZPI_EVENT_RUNTIME_START, pdFALSE, pdTRUE, portMAX_DELAY);
    for (;;) {
        uint32_t command = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &command, portMAX_DELAY);
        if ((command & PREVIEW_CMD_STOP) != 0) {
            xEventGroupSetBits(szpi_system_events, SZPI_EVENT_PREVIEW_STOPPED);
            continue;
        }
        if ((command & PREVIEW_CMD_START) == 0) continue;
        s_stop_latched = false;
        run_preview();
    }
}
