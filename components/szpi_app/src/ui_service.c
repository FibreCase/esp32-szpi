#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "szpi_display.h"
#include "szpi_input.h"
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
    if (event == SZPI_UI_EVENT_NETWORK_BEGIN || event == SZPI_UI_EVENT_NETWORK_CANCEL || event == SZPI_UI_EVENT_NETWORK_FORGET) {
        esp_err_t err = event == SZPI_UI_EVENT_NETWORK_BEGIN ? szpi_app_wifi_provision_begin((value & 1U) != 0, value >> 1) :
            (event == SZPI_UI_EVENT_NETWORK_CANCEL ? szpi_app_wifi_provision_cancel(value) : szpi_app_wifi_forget(value));
        if (err != ESP_OK) ESP_LOGW(TAG, "network command rejected: %s", esp_err_to_name(err));
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

static void update_ui_model(void)
{
    apply_brightness_request();
    poll_imu();
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
        .display_inverted = s_display_inverted,
        .click_count = s_click_count,
        .imu_sequence = s_imu_sample.sequence,
        .accel_g = {s_imu_sample.accel_g[0], s_imu_sample.accel_g[1], s_imu_sample.accel_g[2]},
        .imu_available = s_imu_ready && !s_imu_faulted,
        .imu_valid = s_imu_ready && !s_imu_faulted && s_imu_sample.valid,
        .network_connected = (app_events & SZPI_EVENT_NETWORK_READY) != 0,
        .time_valid = time_valid,
    };
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
    memcpy(model.time_text, time_text, sizeof(model.time_text));
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
