#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "szpi_display.h"
#include "szpi_input.h"
#include "szpi_audio.h"
#include "szpi_ui.h"
#include "szpi_runtime_internal.h"

#define TAG "szpi_ui"
#define TOUCH_PERIOD_MS 20
#define IMU_PERIOD_MS 20
#define IMU_ROTATION_THRESHOLD_G 0.65f
#define IMU_ROTATION_STABLE_SAMPLES 4U
#define UI_LOOP_PERIOD_MS 4
#define FLUSH_WAIT_SLICE_MS 250
#define UI_BRIGHTNESS_PERCENT 50

static lv_display_t *s_lv_display;
static lv_indev_t *s_lv_input;
static bool s_input_ready;
static bool s_touch_faulted;
static bool s_lvgl_initialized;
static uint32_t s_touch_errors;
static uint32_t s_touch_max_read_duration_us;
static uint16_t s_touch_x;
static uint16_t s_touch_y;
static bool s_touch_pressed;
static uint32_t s_click_count;
static bool s_imu_ready;
static bool s_imu_faulted;
static TickType_t s_imu_last_read;
static szpi_imu_sample_t s_imu_sample;
static bool s_display_inverted;
static bool s_orientation_candidate_inverted;
static uint8_t s_orientation_candidate_samples;
static esp_err_t s_orientation_error;
static uint8_t s_brightness = UI_BRIGHTNESS_PERCENT;
static uint8_t s_requested_brightness = UI_BRIGHTNESS_PERCENT;
static uint8_t s_saved_brightness = UI_BRIGHTNESS_PERCENT;
static bool s_brightness_pending;
static bool s_brightness_save_pending;
/* Latest web request, protected by the existing UI status mutex. */
static bool s_web_brightness_pending;
static uint8_t s_web_brightness;

esp_err_t szpi_app_ui_set_brightness(uint8_t percent)
{
    if (percent < 10 || percent > 100) return ESP_ERR_INVALID_ARG;
    if (szpi_ui_status_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(szpi_ui_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (szpi_ui_status.state != SZPI_UI_READY && szpi_ui_status.state != SZPI_UI_TOUCH_FAULT) {
        xSemaphoreGive(szpi_ui_status_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_web_brightness = percent;
    s_web_brightness_pending = true;
    xSemaphoreGive(szpi_ui_status_lock);
    return ESP_OK;
}
static bool s_camera_requested;
static bool s_camera_command_pending;
static esp_err_t s_camera_command_error;
static uint8_t *s_camera_staging;
static lv_image_dsc_t s_camera_image_source;
static uint32_t s_camera_display_frames;
static int64_t s_camera_period_started;
static uint32_t s_camera_period_frames;
static szpi_preview_ack_message_t s_camera_pending_ack;
static bool s_camera_ack_pending;

static void load_brightness(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("display", NVS_READONLY, &handle);
    uint8_t value = UI_BRIGHTNESS_PERCENT;
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, "brightness", &value);
        nvs_close(handle);
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "brightness load failed: %s", esp_err_to_name(err));
    }
    if (err != ESP_OK || value > 100) value = UI_BRIGHTNESS_PERCENT;
    s_saved_brightness = value;
    s_brightness = s_requested_brightness = value < 10 ? 10 : value;
    s_brightness_pending = s_brightness_save_pending = false;
}

static void apply_brightness_request(void)
{
    if (xSemaphoreTake(szpi_ui_status_lock, 0) == pdTRUE) {
        if (s_web_brightness_pending) {
            s_requested_brightness = s_web_brightness;
            s_brightness_pending = s_brightness_save_pending = true;
            s_web_brightness_pending = false;
        }
        xSemaphoreGive(szpi_ui_status_lock);
    }
    if (s_brightness_pending) {
        s_brightness_pending = false;
        esp_err_t err = szpi_display_set_brightness(s_requested_brightness);
        if (err == ESP_OK) s_brightness = s_requested_brightness;
        else ESP_LOGW(TAG, "brightness update failed: %s", esp_err_to_name(err));
    }
    if (!s_brightness_save_pending) return;
    s_brightness_save_pending = false;
    if (s_brightness == s_saved_brightness) return;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("display", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "brightness", s_brightness);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) s_saved_brightness = s_brightness;
    else ESP_LOGW(TAG, "brightness save failed: %s", esp_err_to_name(err));
}

static void set_ui_state(szpi_ui_state_t state, esp_err_t error)
{
    if (szpi_ui_status_lock == NULL || xSemaphoreTake(szpi_ui_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    szpi_ui_status.state = state;
    if (error != ESP_OK) szpi_ui_status.last_error = error;
    else if (state == SZPI_UI_READY || state == SZPI_UI_STOPPED) szpi_ui_status.last_error = ESP_OK;
    xSemaphoreGive(szpi_ui_status_lock);
}

static void primary_action_cb(szpi_ui_event_t event, uint32_t value, void *context)
{
    (void)context;
    if (event == SZPI_UI_EVENT_CAMERA_TEST_START || event == SZPI_UI_EVENT_CAMERA_TEST_STOP) {
        s_camera_requested = event == SZPI_UI_EVENT_CAMERA_TEST_START;
        s_camera_command_pending = true;
        s_camera_command_error = ESP_OK;
        return;
    }
    if (event == SZPI_UI_EVENT_NETWORK_BEGIN || event == SZPI_UI_EVENT_NETWORK_CANCEL || event == SZPI_UI_EVENT_NETWORK_FORGET) {
        esp_err_t err = event == SZPI_UI_EVENT_NETWORK_BEGIN ? szpi_app_wifi_provision_begin((value & 1U) != 0, value >> 1) :
            (event == SZPI_UI_EVENT_NETWORK_CANCEL ? szpi_app_wifi_provision_cancel(value) : szpi_app_wifi_forget(value));
        if (err != ESP_OK) ESP_LOGW(TAG, "network command rejected: %s", esp_err_to_name(err));
        return;
    }
    if (event == SZPI_UI_EVENT_OTA_START) {
        esp_err_t err = szpi_app_ota_start();
        if (err != ESP_OK) ESP_LOGW(TAG, "OTA request rejected: %s", esp_err_to_name(err));
        return;
    }
    if (event == SZPI_UI_EVENT_BRIGHTNESS_CHANGED || event == SZPI_UI_EVENT_BRIGHTNESS_SAVE) {
        if (value <= 100) {
            s_requested_brightness = (uint8_t)(value < 10 ? 10 : value);
            s_brightness_pending = true;
            if (event == SZPI_UI_EVENT_BRIGHTNESS_SAVE) s_brightness_save_pending = true;
        }
        return;
    }
    if (event == SZPI_UI_EVENT_DISPLAY_TEST_START || event == SZPI_UI_EVENT_DISPLAY_TEST_STOP) {
        esp_err_t err = szpi_display_set_test_active(event == SZPI_UI_EVENT_DISPLAY_TEST_START);
        if (err != ESP_OK) ESP_LOGW(TAG, "display test state failed: %s", esp_err_to_name(err));
        return;
    }
    if (event == SZPI_UI_EVENT_SPEAKER_VOLUME_CHANGED ||
        event == SZPI_UI_EVENT_MIC_GAIN_CHANGED) {
        esp_err_t err = event == SZPI_UI_EVENT_SPEAKER_VOLUME_CHANGED ?
            szpi_app_audio_set_volume((uint8_t)value) :
            szpi_app_audio_set_input_gain_percent((uint8_t)value);
        if (err != ESP_OK) ESP_LOGW(TAG, "audio level update rejected: %s", esp_err_to_name(err));
        return;
    }
    if (event == SZPI_UI_EVENT_SPEAKER_VOLUME_SAVE || event == SZPI_UI_EVENT_MIC_GAIN_SAVE) {
        esp_err_t err = szpi_app_audio_save_settings();
        if (err != ESP_OK) ESP_LOGW(TAG, "audio setting save rejected: %s", esp_err_to_name(err));
        return;
    }
    if (event == SZPI_UI_EVENT_AUDIO_TEST_TONE ||
        event == SZPI_UI_EVENT_AUDIO_TEST_CAPTURE || event == SZPI_UI_EVENT_AUDIO_STOP) {
        esp_err_t err = event == SZPI_UI_EVENT_AUDIO_TEST_TONE ? szpi_app_audio_test_tone() :
            (event == SZPI_UI_EVENT_AUDIO_TEST_CAPTURE ? szpi_app_audio_capture_test() : szpi_app_audio_stop());
        if (err != ESP_OK) ESP_LOGW(TAG, "audio test request rejected: %s", esp_err_to_name(err));
        return;
    }
    if (event == SZPI_UI_EVENT_STORAGE_RETRY) {
        esp_err_t err = szpi_app_storage_retry();
        if (err != ESP_OK) ESP_LOGW(TAG, "storage retry rejected: %s", esp_err_to_name(err));
        return;
    }
    if (event == SZPI_UI_EVENT_STORAGE_FORMAT) {
        esp_err_t err = szpi_app_storage_format_confirmed(value);
        if (err != ESP_OK) ESP_LOGW(TAG, "storage format request rejected: %s", esp_err_to_name(err));
        return;
    }
    if (event != SZPI_UI_EVENT_PRIMARY_ACTION) return;
    s_click_count++;
    if (szpi_ui_status_lock == NULL) return;
    if (xSemaphoreTake(szpi_ui_status_lock, 0) == pdTRUE) {
        szpi_ui_status.click_count = s_click_count;
        xSemaphoreGive(szpi_ui_status_lock);
    }
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
            ESP_LOGW(TAG, "touch input disabled after %lu consecutive read failures",
                (unsigned long)s_touch_errors);
        }
        input.x = s_touch_x;
        input.y = s_touch_y;
        input.pressed = false;
        input.consecutive_errors = s_touch_errors;
    } else {
        s_touch_errors = 0;
        if (s_display_inverted) {
            input.x = (uint16_t)(SZPI_DISPLAY_WIDTH - 1U - input.x);
            input.y = (uint16_t)(SZPI_DISPLAY_HEIGHT - 1U - input.y);
        }
    }
    update_touch_status(&input);
    data->point.x = input.x;
    data->point.y = input.y;
    data->state = input.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void update_auto_orientation(const szpi_imu_sample_t *sample)
{
    if (s_orientation_error != ESP_OK) return;
    if (sample == NULL || !sample->fresh || !sample->tilt_reliable) {
        s_orientation_candidate_samples = 0;
        return;
    }

    bool target_inverted;
    if (!s_display_inverted && sample->accel_g[0] <= -IMU_ROTATION_THRESHOLD_G) {
        target_inverted = true;
    } else if (s_display_inverted && sample->accel_g[0] >= IMU_ROTATION_THRESHOLD_G) {
        target_inverted = false;
    } else {
        s_orientation_candidate_samples = 0;
        return;
    }

    if (s_orientation_candidate_samples == 0 ||
        s_orientation_candidate_inverted != target_inverted) {
        s_orientation_candidate_inverted = target_inverted;
        s_orientation_candidate_samples = 1;
    } else if (s_orientation_candidate_samples < IMU_ROTATION_STABLE_SAMPLES) {
        s_orientation_candidate_samples++;
    }
    if (s_orientation_candidate_samples < IMU_ROTATION_STABLE_SAMPLES) return;

    s_orientation_candidate_samples = 0;
    s_orientation_error = szpi_display_set_orientation_inverted(target_inverted);
    if (s_orientation_error != ESP_OK) {
        ESP_LOGE(TAG, "display orientation change failed: %s", esp_err_to_name(s_orientation_error));
        return;
    }
    s_display_inverted = target_inverted;
    lv_obj_invalidate(lv_screen_active());
    ESP_LOGI(TAG, "display orientation changed to %s",
        s_display_inverted ? "inverted" : "upright");
}

static void poll_imu(void)
{
    if (!s_imu_ready || s_imu_faulted) return;
    TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - s_imu_last_read) < pdMS_TO_TICKS(IMU_PERIOD_MS)) return;
    s_imu_last_read = now;

    szpi_imu_sample_t sample = {0};
    esp_err_t err = szpi_imu_read(&sample);
    s_imu_sample = sample;
    if (err == ESP_OK) update_auto_orientation(&sample);
    else s_orientation_candidate_samples = 0;
    if (sample.faulted) {
        s_imu_faulted = true;
        ESP_LOGW(TAG, "IMU readings disabled after %lu consecutive failures",
            (unsigned long)sample.consecutive_errors);
    } else if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "IMU read failed: %s", esp_err_to_name(err));
    }
}

/* Camera frames never become LVGL sources: only the UI-owned staging does. */
static void process_camera_frame(void)
{
    if (s_camera_ack_pending) {
        if (xQueueSend(szpi_preview_ack_queue, &s_camera_pending_ack, 0) != pdTRUE) return;
        s_camera_ack_pending = false;
    }
    szpi_preview_frame_message_t message;
    if (xQueueReceive(szpi_preview_frame_queue, &message, 0) != pdTRUE) return;
    bool displayed = false;
    uint32_t copy_us = 0;
    uint32_t refresh_us = 0;
    if (s_camera_requested && s_camera_staging != NULL &&
        message.frame.width == SZPI_CAMERA_WIDTH && message.frame.height == SZPI_CAMERA_HEIGHT &&
        message.frame.length == SZPI_CAMERA_FRAME_BYTES && message.frame.data != NULL) {
        esp_err_t err = szpi_display_wait_flush(pdMS_TO_TICKS(FLUSH_WAIT_SLICE_MS));
        if (err == ESP_OK) {
            int64_t started = esp_timer_get_time();
            for (size_t y = 0; y < SZPI_CAMERA_HEIGHT; ++y) {
                for (size_t x = 0; x < SZPI_CAMERA_WIDTH; ++x) {
                    size_t source = (y * SZPI_CAMERA_WIDTH + SZPI_CAMERA_WIDTH - 1 - x) * 2;
                    size_t target = (y * SZPI_CAMERA_WIDTH + x) * 2;
                    s_camera_staging[target] = message.frame.data[source + 1];
                    s_camera_staging[target + 1] = message.frame.data[source];
                }
            }
            copy_us = (uint32_t)(esp_timer_get_time() - started);
            if (szpi_ui_camera_test_set_frame(&s_camera_image_source) == SZPI_UI_RESULT_OK) {
                started = esp_timer_get_time();
                lv_refr_now(s_lv_display);
                err = szpi_display_wait_flush(pdMS_TO_TICKS(FLUSH_WAIT_SLICE_MS));
                refresh_us = (uint32_t)(esp_timer_get_time() - started);
                displayed = err == ESP_OK;
            }
        }
        if (err != ESP_OK) s_camera_command_error = err;
    }
    /* Send only after all source reads; never discard the matching ack. */
    szpi_preview_ack_message_t ack = {.token = message.frame.token, .generation = message.generation};
    if (xQueueSend(szpi_preview_ack_queue, &ack, 0) != pdTRUE) {
        s_camera_pending_ack = ack;
        s_camera_ack_pending = true;
        s_camera_command_error = ESP_ERR_INVALID_STATE;
        ESP_LOGE(TAG, "camera acknowledgement queue full");
        (void)szpi_app_camera_preview_request_stop();
    }
    if (displayed) {
        s_camera_display_frames++;
        s_camera_period_frames++;
    }
    int64_t now = esp_timer_get_time();
    if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
        szpi_preview_status.displayed_frames = s_camera_display_frames;
        if (!displayed) szpi_preview_status.dropped_frames++;
        if (copy_us > szpi_preview_status.max_copy_us) szpi_preview_status.max_copy_us = copy_us;
        if (refresh_us > szpi_preview_status.max_refresh_us) szpi_preview_status.max_refresh_us = refresh_us;
        if (now - s_camera_period_started >= 1000000) {
            szpi_preview_status.display_fps_milli = (uint32_t)(s_camera_period_frames * 1000000000ULL /
                (uint64_t)(now - s_camera_period_started));
            s_camera_period_started = now;
            s_camera_period_frames = 0;
        }
        xSemaphoreGive(szpi_preview_status_lock);
    }
}

static void process_camera_command(void)
{
    if (!s_camera_command_pending) return;
    if (!s_camera_requested) {
        s_camera_command_pending = false;
        s_camera_command_error = szpi_app_camera_preview_request_stop();
        return;
    }
    /* A re-entry waits for the previous session to finish, without blocking UI. */
    if ((xEventGroupGetBits(szpi_system_events) & SZPI_EVENT_PREVIEW_STOPPED) == 0) return;
    s_camera_command_pending = false;
    if (s_camera_staging == NULL) {
        s_camera_staging = heap_caps_malloc(SZPI_CAMERA_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_camera_staging == NULL) {
            s_camera_command_error = ESP_ERR_NO_MEM;
            return;
        }
        s_camera_image_source = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                       .w = SZPI_CAMERA_WIDTH, .h = SZPI_CAMERA_HEIGHT, .stride = SZPI_CAMERA_WIDTH * 2},
            .data_size = SZPI_CAMERA_FRAME_BYTES, .data = s_camera_staging,
        };
    }
    s_camera_display_frames = s_camera_period_frames = 0;
    s_camera_period_started = esp_timer_get_time();
    s_camera_command_error = szpi_app_camera_preview_start();
    if (s_camera_command_error != ESP_OK) {
        ESP_LOGW(TAG, "camera start rejected: %s", esp_err_to_name(s_camera_command_error));
    }
}

static void update_ui_model(void)
{
    apply_brightness_request();
    poll_imu();
    process_camera_command();
    process_camera_frame();
    EventBits_t app_events = xEventGroupGetBits(szpi_system_events);
    bool time_valid = (app_events & SZPI_EVENT_TIME_SYNCED) != 0;
    char time_text[6] = "--:--";
    if (time_valid) {
        time_t now = time(NULL);
        struct tm local_time;
        if (now < (time_t)1704067200 || localtime_r(&now, &local_time) == NULL ||
            strftime(time_text, sizeof(time_text), "%H:%M", &local_time) == 0) {
            time_valid = false;
        }
    }
    szpi_ui_model_t model = {
        .display_brightness_percent = s_brightness,
        .speaker_volume_percent = SZPI_AUDIO_DEFAULT_VOLUME_PERCENT,
        .microphone_gain_percent = SZPI_AUDIO_DEFAULT_INPUT_GAIN_PERCENT,
        .audio_state = SZPI_UI_AUDIO_OFFLINE,
        .display_inverted = s_display_inverted,
        .click_count = s_click_count,
        .imu_sequence = s_imu_sample.sequence,
        .accel_g = {s_imu_sample.accel_g[0], s_imu_sample.accel_g[1], s_imu_sample.accel_g[2]},
        .imu_available = s_imu_ready && !s_imu_faulted,
        .imu_valid = s_imu_ready && !s_imu_faulted && s_imu_sample.valid,
        .network_connected = (app_events & SZPI_EVENT_NETWORK_READY) != 0,
        .time_valid = time_valid,
    };
    const esp_app_desc_t *app_description = esp_app_get_description();
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    if (running_partition != NULL) {
        snprintf(model.running_ota_slot, sizeof(model.running_ota_slot), "%s", running_partition->label);
    }
    if (app_description != NULL) {
        snprintf(model.firmware_version, sizeof(model.firmware_version), "%s", app_description->version);
    }
    if (szpi_audio_status_lock != NULL && xSemaphoreTake(szpi_audio_status_lock, 0) == pdTRUE) {
        szpi_audio_service_status_t audio = szpi_audio_status;
        xSemaphoreGive(szpi_audio_status_lock);
        model.speaker_volume_percent = audio.output_volume_percent;
        model.microphone_gain_percent = audio.input_gain_percent;
        switch (audio.state) {
            case SZPI_AUDIO_SERVICE_IDLE: model.audio_state = SZPI_UI_AUDIO_IDLE; break;
            case SZPI_AUDIO_SERVICE_PLAYING_TEST: model.audio_state = SZPI_UI_AUDIO_PLAYING_TEST; break;
            case SZPI_AUDIO_SERVICE_CAPTURE_TEST: model.audio_state = SZPI_UI_AUDIO_CAPTURE_TEST; break;
            case SZPI_AUDIO_SERVICE_PLAYING_CAPTURE_TEST: model.audio_state = SZPI_UI_AUDIO_PLAYING_CAPTURE_TEST; break;
            case SZPI_AUDIO_SERVICE_RECORDING: model.audio_state = SZPI_UI_AUDIO_RECORDING; break;
            case SZPI_AUDIO_SERVICE_PLAYING_FILE: model.audio_state = SZPI_UI_AUDIO_PLAYING_FILE; break;
            case SZPI_AUDIO_SERVICE_COMPLETE: model.audio_state = SZPI_UI_AUDIO_COMPLETE; break;
            case SZPI_AUDIO_SERVICE_FAULT: model.audio_state = SZPI_UI_AUDIO_FAULT; break;
            case SZPI_AUDIO_SERVICE_OFFLINE:
            default: model.audio_state = SZPI_UI_AUDIO_OFFLINE; break;
        }
        model.audio_blocks_processed = audio.blocks_processed;
        model.audio_peak_sample = audio.peak_sample;
        model.audio_rms_sample = audio.rms_sample;
        model.audio_error_code = (uint32_t)audio.last_error;
    }
    if (szpi_storage_status_lock != NULL && xSemaphoreTake(szpi_storage_status_lock, 0) == pdTRUE) {
        szpi_storage_status_t storage = szpi_storage_status;
        xSemaphoreGive(szpi_storage_status_lock);
        switch (storage.state) {
            case SZPI_STORAGE_NO_CARD: model.storage_state = SZPI_UI_STORAGE_NO_CARD; break;
            case SZPI_STORAGE_CARD_READY_NO_FS: model.storage_state = SZPI_UI_STORAGE_CARD_READY_NO_FS; break;
            case SZPI_STORAGE_READY: model.storage_state = SZPI_UI_STORAGE_READY; break;
            case SZPI_STORAGE_BUSY: model.storage_state = SZPI_UI_STORAGE_BUSY; break;
            case SZPI_STORAGE_FORMATTING: model.storage_state = SZPI_UI_STORAGE_FORMATTING; break;
            case SZPI_STORAGE_FAULT: model.storage_state = SZPI_UI_STORAGE_FAULT; break;
            case SZPI_STORAGE_UNINITIALIZED:
            default: model.storage_state = SZPI_UI_STORAGE_UNINITIALIZED; break;
        }
        model.storage_capacity_bytes = storage.capacity_bytes;
        model.storage_free_bytes = storage.free_bytes;
        model.storage_generation = storage.generation;
        model.storage_max_frequency_khz = storage.max_frequency_khz;
        model.storage_fat_type = storage.fat_type;
        model.storage_error_code = (uint32_t)storage.last_error;
    }
    szpi_wifi_status_t wifi = {0};
    if (szpi_app_wifi_get_status(&wifi) == ESP_OK) {
        model.network_connected = wifi.state == SZPI_WIFI_ONLINE && wifi.rssi_valid;
        model.network_details_valid = model.network_connected;
        model.network_rssi = wifi.rssi;
        model.network_channel = wifi.link_info.channel;
        if (model.network_connected) {
            snprintf(model.network_ip, sizeof(model.network_ip), IPSTR, IP2STR(&wifi.ip_info.ip));
            snprintf(model.network_gateway, sizeof(model.network_gateway), IPSTR, IP2STR(&wifi.ip_info.gw));
            snprintf(model.network_netmask, sizeof(model.network_netmask), IPSTR, IP2STR(&wifi.ip_info.netmask));
            const uint8_t *b = wifi.link_info.bssid;
            snprintf(model.network_bssid, sizeof(model.network_bssid), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
        }
        if (wifi.link_info.mac_valid) {
            const uint8_t *m = wifi.link_info.mac;
            snprintf(model.network_mac, sizeof(model.network_mac), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        }
        model.network_supported = wifi.state != SZPI_WIFI_DISABLED;
        model.network_has_config = wifi.has_config;
        model.network_needs_setup = wifi.state == SZPI_WIFI_NO_CONFIG && !wifi.has_config;
        memcpy(model.network_ssid, wifi.ssid, sizeof(model.network_ssid));
        memcpy(model.hostname, wifi.hostname, sizeof(model.hostname));
        model.hostname[sizeof(model.hostname) - 1] = '\0';
        model.provisioning_active = wifi.provisioning.active;
        model.provisioning_state = wifi.provisioning.state;
        model.provisioning_generation = wifi.provisioning.generation;
        model.dpp_ready = wifi.provisioning.dpp_ready;
        memcpy(model.provisioning_message, wifi.provisioning.message, sizeof(model.provisioning_message));
        memcpy(model.setup_ssid, wifi.provisioning.ap_ssid, sizeof(model.setup_ssid));
        memcpy(model.setup_password, wifi.provisioning.ap_password, sizeof(model.setup_password));
        memcpy(model.setup_wifi_qr, wifi.provisioning.wifi_qr, sizeof(model.setup_wifi_qr));
        memcpy(model.setup_dpp_uri, wifi.provisioning.dpp_uri, sizeof(model.setup_dpp_uri));
    }
    szpi_ota_status_t ota = {0};
    if (szpi_app_ota_get_status(&ota) == ESP_OK) {
        model.ota_state = (szpi_ui_ota_state_t)ota.state;
        model.ota_progress_percent = ota.progress_percent;
        model.ota_error_code = (uint32_t)ota.last_error;
    }
    memcpy(model.time_text, time_text, sizeof(model.time_text));
    if (xSemaphoreTake(szpi_preview_status_lock, 0) == pdTRUE) {
        switch (szpi_preview_status.state) {
            case SZPI_CAMERA_PREVIEW_STOPPED: model.camera_state = SZPI_UI_CAMERA_STOPPED; break;
            case SZPI_CAMERA_PREVIEW_STARTING: model.camera_state = SZPI_UI_CAMERA_STARTING; break;
            case SZPI_CAMERA_PREVIEW_RUNNING: model.camera_state = SZPI_UI_CAMERA_RUNNING; break;
            case SZPI_CAMERA_PREVIEW_STOPPING: model.camera_state = SZPI_UI_CAMERA_STOPPING; break;
            case SZPI_CAMERA_PREVIEW_FAULT: model.camera_state = SZPI_UI_CAMERA_FAULT; break;
            default: model.camera_state = SZPI_UI_CAMERA_UNAVAILABLE; break;
        }
        model.camera_fps_milli = szpi_preview_status.display_fps_milli;
        model.camera_error_count = szpi_preview_status.error_count;
        model.camera_error_code = (uint32_t)szpi_preview_status.last_error;
        xSemaphoreGive(szpi_preview_status_lock);
    }
    if (s_camera_command_error != ESP_OK) {
        model.camera_state = SZPI_UI_CAMERA_FAULT;
        model.camera_error_code = (uint32_t)s_camera_command_error;
    }
    szpi_display_test_stats_t stats = {0};
    model.display_test_supported = true;
    if (szpi_display_get_test_stats(&stats) == ESP_OK) {
        model.display_test_valid = stats.valid;
        model.display_fps_x10 = stats.fps_x10;
        model.display_frame_avg_us = stats.frame_avg_us;
        model.display_frame_max_us = stats.frame_max_us;
        model.display_lvgl_avg_us = stats.lvgl_avg_us;
        model.display_gap_avg_us = stats.gap_avg_us;
    }
    if (szpi_ui_status_lock != NULL && xSemaphoreTake(szpi_ui_status_lock, 0) == pdTRUE) {
        szpi_ui_status.click_count = s_click_count;
        xSemaphoreGive(szpi_ui_status_lock);
    }
    (void)szpi_ui_update(&model);
    if (xSemaphoreTake(szpi_ui_status_lock, 0) == pdTRUE) {
        szpi_ui_status.brightness_percent = s_brightness;
        xSemaphoreGive(szpi_ui_status_lock);
    }
}

static esp_err_t initialize_ui(void)
{
    set_ui_state(SZPI_UI_STARTING, ESP_OK);
    load_brightness();
    s_touch_faulted = false;
    s_touch_errors = 0;
    s_touch_max_read_duration_us = 0;
    s_touch_pressed = false;
    s_touch_x = 0;
    s_touch_y = 0;
    s_click_count = 0;
    s_imu_ready = false;
    s_imu_faulted = false;
    s_imu_last_read = 0;
    s_imu_sample = (szpi_imu_sample_t){0};
    s_display_inverted = false;
    s_orientation_candidate_inverted = false;
    s_orientation_candidate_samples = 0;
    s_orientation_error = ESP_OK;
    if (szpi_ui_status_lock != NULL && xSemaphoreTake(szpi_ui_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        szpi_ui_status.click_count = 0;
        s_web_brightness_pending = false;
        szpi_ui_status.brightness_percent = UI_BRIGHTNESS_PERCENT;
        szpi_ui_status.touch_x = 0;
        szpi_ui_status.touch_y = 0;
        szpi_ui_status.touch_pressed = false;
        szpi_ui_status.touch_faulted = false;
        szpi_ui_status.touch_errors = 0;
        szpi_ui_status.touch_max_read_duration_us = 0;
        xSemaphoreGive(szpi_ui_status_lock);
    }

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
    s_input_ready = err == ESP_OK;
    if (!s_input_ready) {
        s_touch_faulted = true;
        xEventGroupSetBits(szpi_system_events, SZPI_EVENT_TOUCH_FAULT);
        ESP_LOGW(TAG, "touch unavailable; display remains active: %s", esp_err_to_name(err));
    }

    szpi_ui_result_t ui_result = szpi_ui_create(primary_action_cb, NULL);
    if (ui_result == SZPI_UI_RESULT_NO_MEMORY) return ESP_ERR_NO_MEM;
    if (ui_result == SZPI_UI_RESULT_INVALID_STATE) return ESP_ERR_INVALID_STATE;
    if (ui_result != SZPI_UI_RESULT_OK) return ESP_ERR_INVALID_ARG;
    lv_refr_now(s_lv_display);
    err = szpi_display_wait_flush(pdMS_TO_TICKS(1000));
    if (err != ESP_OK) return err;
    err = szpi_display_set_brightness(s_brightness);
    if (err != ESP_OK) return err;

    szpi_imu_info_t imu_info = {0};
    err = szpi_imu_init(&imu_info);
    s_imu_ready = err == ESP_OK;
    s_imu_last_read = xTaskGetTickCount();
    if (!s_imu_ready) {
        ESP_LOGW(TAG, "IMU unavailable; acceleration readout disabled: %s", esp_err_to_name(err));
    }

    set_ui_state(s_touch_faulted ? SZPI_UI_TOUCH_FAULT : SZPI_UI_READY, ESP_OK);
    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_UI_READY);
    ESP_LOGI(TAG, "initial UI refresh completed; brightness=%u%%", s_brightness);
    return ESP_OK;
}

static bool cleanup_ui(void)
{
    /* Preserve the latest settings even if STOP arrives before the next UI
     * iteration or while a slider is still being dragged. */
    if (s_lvgl_initialized) {
        s_brightness_save_pending = true;
        apply_brightness_request();
        esp_err_t save_err = szpi_app_audio_save_settings();
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "audio settings save on UI stop rejected: %s", esp_err_to_name(save_err));
        }
    }
    s_camera_requested = false;
    s_camera_command_pending = false;
    if (szpi_preview_task_handle != NULL) {
        (void)szpi_app_camera_preview_request_stop();
        TickType_t started = xTaskGetTickCount();
        while ((xEventGroupGetBits(szpi_system_events) & SZPI_EVENT_PREVIEW_STOPPED) == 0) {
            process_camera_frame();
            if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(5500)) {
                ESP_LOGE(TAG, "camera stop timed out; preview service retains camera resources");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        process_camera_frame();
    }
    (void)szpi_ui_camera_test_set_frame(NULL);
    while (s_lv_display != NULL &&
        szpi_display_wait_flush(pdMS_TO_TICKS(FLUSH_WAIT_SLICE_MS)) == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "waiting for LCD DMA before releasing display resources");
    }
    if (s_input_ready) {
        esp_err_t err = szpi_input_deinit();
        if (err != ESP_OK) ESP_LOGW(TAG, "touch deinit failed: %s", esp_err_to_name(err));
        s_input_ready = false;
    }
    if (s_imu_ready) {
        esp_err_t imu_err = szpi_imu_deinit();
        if (imu_err != ESP_OK) ESP_LOGW(TAG, "IMU deinit failed: %s", esp_err_to_name(imu_err));
        s_imu_ready = false;
    }
    if (s_lv_input != NULL) {
        lv_indev_delete(s_lv_input);
        s_lv_input = NULL;
    }
    if (s_lvgl_initialized) szpi_ui_destroy();
    heap_caps_free(s_camera_staging);
    s_camera_staging = NULL;
    s_camera_image_source = (lv_image_dsc_t){0};
    s_camera_requested = s_camera_command_pending = false;
    s_camera_command_error = ESP_OK;
    esp_err_t err = szpi_display_set_brightness(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "backlight off failed: %s", esp_err_to_name(err));
    }
    err = szpi_display_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "display deinit failed: %s", esp_err_to_name(err));
    }
    s_lv_display = NULL;
    if (s_lvgl_initialized) {
        lv_deinit();
        s_lvgl_initialized = false;
    }
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
            (void)cleanup_ui();
            set_ui_state(SZPI_UI_DISPLAY_FAULT, err);
            xEventGroupSetBits(szpi_system_events, SZPI_EVENT_DISPLAY_FAULT);
            signal_stopped();
            ESP_LOGE(TAG, "UI initialization failed: %s", esp_err_to_name(err));
            continue;
        }

        TickType_t last_wake = xTaskGetTickCount();
        bool stop_requested = false;
        while (!stop_requested) {
            update_ui_model();
            (void)lv_timer_handler();
            esp_err_t display_error = szpi_display_get_last_error();
            if (s_orientation_error != ESP_OK) display_error = s_orientation_error;
            if (display_error != ESP_OK) {
                set_ui_state(SZPI_UI_DISPLAY_FAULT, display_error);
                xEventGroupSetBits(szpi_system_events, SZPI_EVENT_DISPLAY_FAULT);
                break;
            }

            uint32_t flush_timeouts = szpi_display_get_flush_timeout_count();
            if (szpi_ui_status_lock != NULL && xSemaphoreTake(szpi_ui_status_lock, 0) == pdTRUE) {
                szpi_ui_status.flush_timeouts = flush_timeouts;
                szpi_ui_status.touch_faulted = s_touch_faulted;
                xSemaphoreGive(szpi_ui_status_lock);
            }
            uint32_t value = 0;
            stop_requested = process_ui_command(0, &value);
            if ((value & SZPI_UI_CMD_START) != 0) ESP_LOGW(TAG, "ignoring duplicate UI start request");
            if (s_touch_faulted) set_ui_state(SZPI_UI_TOUCH_FAULT, ESP_FAIL);
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(UI_LOOP_PERIOD_MS));
        }

        if (stop_requested) set_ui_state(SZPI_UI_STOPPING, ESP_OK);
        (void)cleanup_ui();
        if (stop_requested) set_ui_state(SZPI_UI_STOPPED, ESP_OK);
        signal_stopped();
    }
}
