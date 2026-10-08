#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "szpi_display.h"
#include "szpi_input.h"
#include "ui_pages.h"
#include "szpi_runtime_internal.h"

#define TAG "szpi_ui"
#define TOUCH_PERIOD_MS 20
#define UI_LOOP_PERIOD_MS 4
#define IMU_POLL_PERIOD_MS 20
#define INPUT_UI_PERIOD_MS 100
#define FLUSH_WAIT_SLICE_MS 250

static lv_display_t *s_lv_display;
static lv_indev_t *s_lv_input;
static bool s_input_ready;
static bool s_touch_faulted;
static bool s_display_faulted;
static bool s_lvgl_initialized;
static uint32_t s_touch_errors;
static uint32_t s_touch_max_read_duration_us;
static uint16_t s_touch_x;
static uint16_t s_touch_y;
static bool s_touch_pressed;
static bool s_imu_ready;
static bool s_boot_ready;
static uint32_t s_last_imu_poll;
static uint32_t s_last_input_ui_update;
static uint32_t s_boot_short_count;
static uint32_t s_boot_long_count;
static szpi_imu_info_t s_imu_info;
static szpi_imu_sample_t s_imu_sample;
static szpi_boot_state_t s_boot_state;
static int64_t s_started_at_us;
static uint8_t *s_camera_staging;
static uint32_t s_preview_display_period_frames;
static int64_t s_preview_display_period_started_us;

void szpi_ui_service_retry_inputs(void)
{
    if (s_imu_ready) {
        (void)szpi_imu_deinit();
        s_imu_ready = false;
    }
    memset(&s_imu_sample, 0, sizeof(s_imu_sample));
    esp_err_t err = szpi_imu_init(&s_imu_info);
    s_imu_ready = err == ESP_OK;
    if (!s_imu_ready) ESP_LOGW(TAG, "IMU retry failed: %s", esp_err_to_name(err));
}

#define CAMERA_UI_REFRESH_TIMEOUT_MS 250
#define CAMERA_UI_STOP_TIMEOUT_MS 5000

static void set_ui_state(szpi_ui_state_t state, esp_err_t error)
{
    if (szpi_ui_status_lock == NULL || xSemaphoreTake(szpi_ui_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    szpi_ui_status.state = state;
    if (error != ESP_OK) szpi_ui_status.last_error = error;
    else if (state == SZPI_UI_READY || state == SZPI_UI_STOPPED) szpi_ui_status.last_error = ESP_OK;
    xSemaphoreGive(szpi_ui_status_lock);
}

static void update_touch_status(const szpi_input_state_t *input)
{
    s_touch_x = input->x;
    s_touch_y = input->y;
    s_touch_pressed = input->pressed;
    s_touch_errors = input->consecutive_errors;
    if (input->read_duration_us > s_touch_max_read_duration_us) {
        s_touch_max_read_duration_us = input->read_duration_us;
    }
    if (szpi_ui_status_lock != NULL && xSemaphoreTake(szpi_ui_status_lock, 0) == pdTRUE) {
        szpi_ui_status.touch_x = s_touch_x;
        szpi_ui_status.touch_y = s_touch_y;
        szpi_ui_status.touch_pressed = s_touch_pressed;
        szpi_ui_status.touch_errors = s_touch_errors;
        szpi_ui_status.touch_max_read_duration_us = s_touch_max_read_duration_us;
        szpi_ui_status.touch_faulted = s_touch_faulted;
        xSemaphoreGive(szpi_ui_status_lock);
    }
}

static void input_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (!s_input_ready || s_touch_faulted) {
        data->point.x = s_touch_x;
        data->point.y = s_touch_y;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    szpi_input_state_t input = {0};
    esp_err_t err = szpi_input_read(&input);
    if (err != ESP_OK) {
        s_touch_errors++;
        if (input.faulted || s_touch_errors >= 5) {
            s_touch_faulted = true;
            (void)szpi_input_deinit();
            xEventGroupSetBits(szpi_system_events, SZPI_EVENT_TOUCH_FAULT);
            ESP_LOGW(TAG, "touch input disabled after %" PRIu32 " consecutive read failures", s_touch_errors);
        }
        input.x = s_touch_x;
        input.y = s_touch_y;
        input.pressed = false;
        input.consecutive_errors = s_touch_errors;
    } else {
        s_touch_errors = 0;
    }
    update_touch_status(&input);
    data->point.x = input.x;
    data->point.y = input.y;
    data->state = input.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void poll_optional_inputs(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_boot_ready) {
        szpi_boot_state_t state = {0};
        esp_err_t err = szpi_boot_button_read(&state);
        if (err == ESP_OK) {
            s_boot_state = state;
            if ((state.events & SZPI_BOOT_EVENT_SHORT_PRESS) != 0) s_boot_short_count++;
            if ((state.events & SZPI_BOOT_EVENT_LONG_PRESS) != 0) s_boot_long_count++;
        } else {
            ESP_LOGW(TAG, "BOOT read failed: %s", esp_err_to_name(err));
            (void)szpi_boot_button_deinit();
            s_boot_ready = false;
        }
    }
    if (s_imu_ready && (uint32_t)(now_ms - s_last_imu_poll) >= IMU_POLL_PERIOD_MS) {
        s_last_imu_poll = now_ms;
        szpi_imu_sample_t sample = {0};
        esp_err_t err = szpi_imu_read(&sample);
        s_imu_sample = sample;
        if (err != ESP_OK && sample.faulted) {
            ESP_LOGW(TAG, "IMU disabled after %" PRIu32 " consecutive read failures", sample.consecutive_errors);
            (void)szpi_imu_deinit();
            s_imu_ready = false;
        }
    }
    if ((uint32_t)(now_ms - s_last_input_ui_update) < INPUT_UI_PERIOD_MS) return;
    s_last_input_ui_update = now_ms;
    szpi_ui_pages_update_inputs(s_imu_ready, &s_imu_info, &s_imu_sample, s_boot_ready,
        &s_boot_state, s_boot_short_count, s_boot_long_count);
}

static void account_displayed_frame(uint32_t refresh_us)
{
    int64_t now_us = esp_timer_get_time();
    s_preview_display_period_frames++;
    uint32_t fps_milli = 0;
    if (s_preview_display_period_started_us != 0 && now_us > s_preview_display_period_started_us) {
        uint64_t elapsed = (uint64_t)(now_us - s_preview_display_period_started_us);
        fps_milli = (uint32_t)(((uint64_t)s_preview_display_period_frames * 1000000000ULL) / elapsed);
    }
    if (s_preview_display_period_started_us == 0 || now_us - s_preview_display_period_started_us >= 1000000) {
        if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
            szpi_preview_status.display_fps_milli = fps_milli;
            xSemaphoreGive(szpi_preview_status_lock);
        }
        s_preview_display_period_frames = 0;
        s_preview_display_period_started_us = now_us;
    }
    if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
        szpi_preview_status.displayed_frames++;
        if (refresh_us > szpi_preview_status.max_refresh_us) szpi_preview_status.max_refresh_us = refresh_us;
        xSemaphoreGive(szpi_preview_status_lock);
    }
}

static void process_preview_frame(void)
{
    if (szpi_preview_frame_queue == NULL || s_camera_staging == NULL) return;
    szpi_preview_frame_message_t message;
    if (xQueuePeek(szpi_preview_frame_queue, &message, 0) != pdTRUE) return;
    // Do not overwrite the image source until the previous LCD DMA has returned
    // its draw buffer. The frame remains in the queue while this wait completes.
    esp_err_t err = szpi_display_wait_flush(pdMS_TO_TICKS(CAMERA_UI_REFRESH_TIMEOUT_MS));
    if (err == ESP_ERR_TIMEOUT) return;
    if (err != ESP_OK) return;
    if (xQueueReceive(szpi_preview_frame_queue, &message, 0) != pdTRUE) return;
    if (message.frame.data == NULL || message.frame.length != SZPI_CAMERA_FRAME_BYTES ||
        message.frame.width != SZPI_CAMERA_WIDTH || message.frame.height != SZPI_CAMERA_HEIGHT) {
        ESP_LOGE(TAG, "invalid preview message token=%" PRIu32, message.frame.token);
        szpi_preview_ack_message_t ack = {.token = message.frame.token, .generation = message.generation};
        (void)xQueueSend(szpi_preview_ack_queue, &ack, 0);
        return;
    }
    if (!szpi_ui_pages_camera_active()) {
        szpi_preview_ack_message_t ack = {.token = message.frame.token, .generation = message.generation};
        (void)xQueueSend(szpi_preview_ack_queue, &ack, 0);
        if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
            szpi_preview_status.dropped_frames++;
            xSemaphoreGive(szpi_preview_status_lock);
        }
        return;
    }
    int64_t copy_started = esp_timer_get_time();
    // Mirror the front-facing preview horizontally while converting the
    // camera's big-endian RGB565 pixels to LVGL's native little-endian form.
    for (uint16_t y = 0; y < SZPI_CAMERA_HEIGHT; ++y) {
        for (uint16_t x = 0; x < SZPI_CAMERA_WIDTH; ++x) {
            size_t source_offset = ((size_t)y * SZPI_CAMERA_WIDTH +
                (SZPI_CAMERA_WIDTH - 1U - x)) * 2U;
            size_t destination_offset = ((size_t)y * SZPI_CAMERA_WIDTH + x) * 2U;
            s_camera_staging[destination_offset] = message.frame.data[source_offset + 1U];
            s_camera_staging[destination_offset + 1U] = message.frame.data[source_offset];
        }
    }
    uint32_t copy_us = (uint32_t)(esp_timer_get_time() - copy_started);
    if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
        if (copy_us > szpi_preview_status.max_copy_us) szpi_preview_status.max_copy_us = copy_us;
        xSemaphoreGive(szpi_preview_status_lock);
    }
    szpi_ui_pages_show_camera_frame();
    szpi_preview_ack_message_t ack = {.token = message.frame.token, .generation = message.generation};
    if (xQueueSend(szpi_preview_ack_queue, &ack, 0) != pdTRUE) {
        ESP_LOGE(TAG, "preview ack queue unexpectedly full; retaining camera frame");
        return;
    }
    int64_t refresh_started = esp_timer_get_time();
    lv_refr_now(s_lv_display);
    err = szpi_display_wait_flush(pdMS_TO_TICKS(CAMERA_UI_REFRESH_TIMEOUT_MS));
    uint32_t refresh_us = (uint32_t)(esp_timer_get_time() - refresh_started);
    if (err == ESP_OK) account_displayed_frame(refresh_us);
    else ESP_LOGE(TAG, "preview refresh failed: %s", esp_err_to_name(err));
}

static esp_err_t initialize_ui(void)
{
    set_ui_state(SZPI_UI_STARTING, ESP_OK);
    s_display_faulted = false;
    s_touch_faulted = false;
    s_touch_errors = 0;
    s_touch_max_read_duration_us = 0;
    s_touch_pressed = false;
    s_input_ready = false;
    s_touch_x = 0;
    s_touch_y = 0;
    s_imu_ready = false;
    s_boot_ready = false;
    s_last_imu_poll = 0;
    s_last_input_ui_update = 0;
    s_boot_short_count = 0;
    s_boot_long_count = 0;
    memset(&s_imu_info, 0, sizeof(s_imu_info));
    memset(&s_imu_sample, 0, sizeof(s_imu_sample));
    memset(&s_boot_state, 0, sizeof(s_boot_state));
    s_preview_display_period_frames = 0;
    s_preview_display_period_started_us = 0;
    lv_init();
    s_lvgl_initialized = true;
    esp_err_t err = szpi_display_init(&s_lv_display);
    if (err != ESP_OK) return err;
    s_lv_input = lv_indev_create();
    if (s_lv_input == NULL) return ESP_ERR_NO_MEM;
    lv_indev_set_type(s_lv_input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_lv_input, input_read_cb);
    lv_timer_set_period(lv_indev_get_read_timer(s_lv_input), TOUCH_PERIOD_MS);

    err = szpi_input_init();
    if (err == ESP_OK) s_input_ready = true;
    else {
        s_input_ready = false;
        s_touch_faulted = true;
        xEventGroupSetBits(szpi_system_events, SZPI_EVENT_TOUCH_FAULT);
        ESP_LOGW(TAG, "touch unavailable; display remains active: %s", esp_err_to_name(err));
    }
    err = szpi_imu_init(&s_imu_info);
    if (err == ESP_OK) s_imu_ready = true;
    else ESP_LOGW(TAG, "IMU unavailable; UI remains active: %s", esp_err_to_name(err));
    err = szpi_boot_button_init();
    if (err == ESP_OK) s_boot_ready = true;
    else ESP_LOGW(TAG, "BOOT input unavailable; UI remains active: %s", esp_err_to_name(err));
    szpi_ui_pages_create();
    s_camera_staging = heap_caps_malloc(SZPI_CAMERA_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_camera_staging != NULL) {
        szpi_ui_pages_set_camera_source(s_camera_staging);
    } else {
        ESP_LOGE(TAG, "camera preview disabled: cannot allocate %u-byte PSRAM staging buffer",
            (unsigned)SZPI_CAMERA_FRAME_BYTES);
    }
    s_started_at_us = esp_timer_get_time();
    lv_refr_now(s_lv_display);
    err = szpi_display_wait_flush(pdMS_TO_TICKS(1000));
    if (err != ESP_OK) return err;
    err = szpi_display_set_brightness(szpi_ui_pages_brightness());
    if (err != ESP_OK) return err;
    set_ui_state(s_touch_faulted ? SZPI_UI_TOUCH_FAULT : SZPI_UI_READY, ESP_OK);
    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_UI_READY);
    ESP_LOGI(TAG, "initial UI refresh completed; backlight=%u%%", (unsigned)szpi_ui_pages_brightness());
    return ESP_OK;
}

static bool stop_preview_before_ui_cleanup(void)
{
    szpi_camera_preview_status_t preview = {0};
    if (szpi_app_camera_preview_get_status(&preview) != ESP_OK) return false;
    if (preview.state == SZPI_CAMERA_PREVIEW_STOPPED || preview.state == SZPI_CAMERA_PREVIEW_UNAVAILABLE ||
        (preview.state == SZPI_CAMERA_PREVIEW_FAULT && !preview.frame_outstanding)) return true;
    if (szpi_app_camera_preview_request_stop() != ESP_OK) return false;
    TickType_t started = xTaskGetTickCount();
    while ((xTaskGetTickCount() - started) < pdMS_TO_TICKS(CAMERA_UI_STOP_TIMEOUT_MS)) {
        process_preview_frame();
        if (s_lv_display != NULL) (void)lv_timer_handler();
        if (szpi_app_camera_preview_get_status(&preview) == ESP_OK &&
            (preview.state == SZPI_CAMERA_PREVIEW_STOPPED || preview.state == SZPI_CAMERA_PREVIEW_UNAVAILABLE ||
             (preview.state == SZPI_CAMERA_PREVIEW_FAULT && !preview.frame_outstanding))) return true;
        vTaskDelay(pdMS_TO_TICKS(UI_LOOP_PERIOD_MS));
    }
    ESP_LOGE(TAG, "camera preview did not stop; retaining UI image and PSRAM staging resources");
    return false;
}

static bool cleanup_ui(void)
{
    if (!stop_preview_before_ui_cleanup()) return false;
    if (s_lv_display != NULL) {
        while (szpi_display_wait_flush(pdMS_TO_TICKS(FLUSH_WAIT_SLICE_MS)) == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "stop is waiting for LCD DMA; buffer and driver remain owned until completion");
        }
    }
    if (s_input_ready) {
        esp_err_t err = szpi_input_deinit();
        if (err != ESP_OK) ESP_LOGW(TAG, "touch deinit failed: %s", esp_err_to_name(err));
        s_input_ready = false;
    }
    if (s_imu_ready) {
        esp_err_t err = szpi_imu_deinit();
        if (err != ESP_OK) ESP_LOGW(TAG, "IMU deinit failed: %s", esp_err_to_name(err));
        s_imu_ready = false;
    }
    if (s_boot_ready) {
        esp_err_t err = szpi_boot_button_deinit();
        if (err != ESP_OK) ESP_LOGW(TAG, "BOOT deinit failed: %s", esp_err_to_name(err));
        s_boot_ready = false;
    }
    if (s_lv_input != NULL) { lv_indev_delete(s_lv_input); s_lv_input = NULL; }
    szpi_ui_pages_destroy();
    esp_err_t err = szpi_display_set_brightness(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) ESP_LOGW(TAG, "backlight off failed: %s", esp_err_to_name(err));
    err = szpi_display_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "display deinit failed: %s", esp_err_to_name(err));
    s_lv_display = NULL;
    if (s_lvgl_initialized) {
        lv_deinit();
        s_lvgl_initialized = false;
    }
    if (s_camera_staging != NULL) { heap_caps_free(s_camera_staging); s_camera_staging = NULL; }
    return true;
}

static bool process_ui_command(TickType_t wait_ticks, uint32_t *command)
{
    uint32_t value = 0;
    (void)xTaskNotifyWait(0, UINT32_MAX, &value, wait_ticks);
    if (command != NULL) *command = value;
    return (value & SZPI_UI_CMD_STOP) != 0;
}

static void signal_stopped(void)
{
    xEventGroupClearBits(szpi_system_events, SZPI_EVENT_UI_READY);
    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_UI_STOPPED);
}

void szpi_ui_service_task(void *context)
{
    (void)context;
    (void)xEventGroupWaitBits(szpi_system_events, SZPI_EVENT_RUNTIME_START, pdFALSE, pdTRUE, portMAX_DELAY);
    bool initial_start = true;
    for (;;) {
        uint32_t command = 0;
        if (!initial_start) {
            (void)process_ui_command(portMAX_DELAY, &command);
            if ((command & SZPI_UI_CMD_START) == 0) {
                if ((command & SZPI_UI_CMD_STOP) != 0) {
                    set_ui_state(SZPI_UI_STOPPED, ESP_OK);
                    signal_stopped();
                }
                continue;
            }
        }
        initial_start = false;
        xEventGroupClearBits(szpi_system_events, SZPI_EVENT_UI_STOPPED | SZPI_EVENT_DISPLAY_FAULT | SZPI_EVENT_TOUCH_FAULT);
        esp_err_t err = initialize_ui();
        if (err != ESP_OK) {
            s_display_faulted = true;
            cleanup_ui();
            set_ui_state(SZPI_UI_DISPLAY_FAULT, err);
            xEventGroupSetBits(szpi_system_events, SZPI_EVENT_DISPLAY_FAULT);
            signal_stopped();
            ESP_LOGE(TAG, "UI initialization failed: %s", esp_err_to_name(err));
            continue;
        }
        TickType_t last_wake = xTaskGetTickCount();
        bool stop_requested = false;
        while (!stop_requested && !s_display_faulted) {
            poll_optional_inputs();
            process_preview_frame();
            uint32_t runtime_seconds = (uint32_t)((esp_timer_get_time() - s_started_at_us) / 1000000);
            szpi_ui_pages_update_status(runtime_seconds, s_touch_pressed, s_touch_x, s_touch_y, s_touch_faulted);
            (void)lv_timer_handler();
            esp_err_t display_error = szpi_display_get_last_error();
            uint32_t flush_timeouts = szpi_display_get_flush_timeout_count();
            if (szpi_ui_status_lock != NULL && xSemaphoreTake(szpi_ui_status_lock, 0) == pdTRUE) {
                szpi_ui_status.flush_timeouts = flush_timeouts;
                xSemaphoreGive(szpi_ui_status_lock);
            }
            if (display_error != ESP_OK) {
                s_display_faulted = true;
                ESP_LOGE(TAG, "display entered fault state: %s", esp_err_to_name(display_error));
            }
            uint32_t command = 0;
            // The periodic delay below is the sole pacing wait. A second
            // blocking notification wait would add latency to LVGL timers.
            stop_requested = process_ui_command(0, &command);
            if ((command & SZPI_UI_CMD_START) != 0) ESP_LOGW(TAG, "ignoring duplicate UI start request");
            if (s_touch_faulted) set_ui_state(SZPI_UI_TOUCH_FAULT, ESP_FAIL);
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(UI_LOOP_PERIOD_MS));
        }
        if (stop_requested) set_ui_state(SZPI_UI_STOPPING, ESP_OK);
        else {
            set_ui_state(SZPI_UI_DISPLAY_FAULT, ESP_FAIL);
            xEventGroupSetBits(szpi_system_events, SZPI_EVENT_DISPLAY_FAULT);
        }
        // Keep buffers and the panel driver alive until the DMA callback confirms completion.
        if (!cleanup_ui()) {
            set_ui_state(SZPI_UI_FAILED, ESP_ERR_TIMEOUT);
            // The camera may still own a frame referenced by the UI. Keep the
            // task, image descriptor, staging memory, and display alive.
            for (;;) {
                process_preview_frame();
                (void)lv_timer_handler();
                vTaskDelay(pdMS_TO_TICKS(UI_LOOP_PERIOD_MS));
            }
        }
        if (stop_requested) set_ui_state(SZPI_UI_STOPPED, ESP_OK);
        signal_stopped();
    }
}
