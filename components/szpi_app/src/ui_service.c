#include <inttypes.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "szpi_display.h"
#include "szpi_input.h"
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
static uint32_t s_click_count;
static uint32_t s_touch_errors;
static uint32_t s_touch_max_read_duration_us;
static uint32_t s_last_status_update;
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
static uint8_t s_brightness = 50;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_counter_label;
static lv_obj_t *s_brightness_label;
static lv_obj_t *s_touch_label;
static lv_obj_t *s_runtime_label;
static lv_obj_t *s_touch_fault_label;
static lv_obj_t *s_home_screen;
static lv_obj_t *s_camera_screen;
static lv_obj_t *s_camera_image;
static lv_obj_t *s_camera_status_label;
static lv_obj_t *s_camera_metrics_label;
static lv_obj_t *s_camera_control_label;
static lv_obj_t *s_input_screen;
static lv_obj_t *s_input_identity_label;
static lv_obj_t *s_input_accel_label;
static lv_obj_t *s_input_gyro_label;
static lv_obj_t *s_input_tilt_label;
static lv_obj_t *s_input_status_label;
static lv_obj_t *s_boot_status_label;
static lv_image_dsc_t s_camera_image_descriptor;
static uint8_t *s_camera_staging;
static bool s_camera_page_active;
static uint32_t s_preview_display_period_frames;
static int64_t s_preview_display_period_started_us;

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

static void click_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    s_click_count++;
    if (s_counter_label != NULL) lv_label_set_text_fmt(s_counter_label, "Tap %" PRIu32, s_click_count);
    if (szpi_ui_status_lock != NULL && xSemaphoreTake(szpi_ui_status_lock, 0) == pdTRUE) {
        szpi_ui_status.click_count = s_click_count;
        xSemaphoreGive(szpi_ui_status_lock);
    }
}

static void preview_control_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    if (s_camera_staging == NULL) return;
    szpi_camera_preview_status_t status = {0};
    if (szpi_app_camera_preview_get_status(&status) != ESP_OK) return;
    esp_err_t err;
    if (status.state == SZPI_CAMERA_PREVIEW_RUNNING || status.state == SZPI_CAMERA_PREVIEW_STARTING ||
        status.state == SZPI_CAMERA_PREVIEW_STOPPING) {
        err = szpi_app_camera_preview_request_stop();
    } else {
        err = szpi_app_camera_preview_start();
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "camera preview command failed: %s", esp_err_to_name(err));
}

static void camera_back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    (void)szpi_app_camera_preview_request_stop();
    s_camera_page_active = false;
    lv_screen_load(s_home_screen);
}

static void camera_page_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    s_camera_page_active = true;
    lv_screen_load(s_camera_screen);
}

static void input_page_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_screen_load(s_input_screen);
}

static void input_back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_screen_load(s_home_screen);
}

static void input_retry_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    if (s_imu_ready) {
        (void)szpi_imu_deinit();
        s_imu_ready = false;
    }
    memset(&s_imu_sample, 0, sizeof(s_imu_sample));
    esp_err_t err = szpi_imu_init(&s_imu_info);
    s_imu_ready = err == ESP_OK;
    if (!s_imu_ready) ESP_LOGW(TAG, "IMU retry failed: %s", esp_err_to_name(err));
}

static void brightness_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *slider = lv_event_get_target(event);
    int32_t value = lv_slider_get_value(slider);
    if (value < 10 || value > 100) return;
    esp_err_t err = szpi_display_set_brightness((uint8_t)value);
    if (err == ESP_OK) {
        s_brightness = (uint8_t)value;
        lv_label_set_text_fmt(s_brightness_label, "%" PRId32 "%%", value);
        if (szpi_ui_status_lock != NULL && xSemaphoreTake(szpi_ui_status_lock, 0) == pdTRUE) {
            szpi_ui_status.brightness_percent = s_brightness;
            xSemaphoreGive(szpi_ui_status_lock);
        }
    } else {
        ESP_LOGW(TAG, "backlight update failed: %s", esp_err_to_name(err));
    }
}

static lv_obj_t *create_swatch(lv_obj_t *parent, int32_t x, const char *name, lv_color_t color)
{
    lv_obj_t *swatch = lv_obj_create(parent);
    lv_obj_set_pos(swatch, x, 54);
    lv_obj_set_size(swatch, 68, 24);
    lv_obj_set_style_bg_color(swatch, color, 0);
    lv_obj_set_style_border_width(swatch, 0, 0);
    lv_obj_set_style_radius(swatch, 5, 0);
    lv_obj_set_style_pad_all(swatch, 0, 0);
    lv_obj_t *label = lv_label_create(swatch);
    lv_label_set_text(label, name);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    return swatch;
}

static lv_obj_t *create_panel(lv_obj_t *parent, int32_t x, int32_t y,
                              int32_t width, int32_t height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1B2935), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static lv_obj_t *create_section_label(lv_obj_t *parent, const char *text,
                                      int32_t x, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(0x91A4B4), 0);
    return label;
}

static void format_fixed_value(char *destination, size_t size, float value,
                               uint32_t scale, uint8_t decimal_places)
{
    float scaled_float = value * (float)scale;
    int32_t scaled = (int32_t)(scaled_float >= 0.0f ?
        scaled_float + 0.5f : scaled_float - 0.5f);
    bool negative = scaled < 0;
    uint32_t magnitude = negative ? (uint32_t)(-(int64_t)scaled) : (uint32_t)scaled;
    uint32_t whole = magnitude / scale;
    uint32_t fraction = magnitude % scale;

    if (decimal_places == 1) {
        (void)lv_snprintf(destination, size, "%c%" PRIu32 ".%01" PRIu32,
            negative ? '-' : '+', whole, fraction);
    } else {
        (void)lv_snprintf(destination, size, "%c%" PRIu32 ".%02" PRIu32,
            negative ? '-' : '+', whole, fraction);
    }
}

static void make_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    s_home_screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "SZPI DEVICE CHECK");
    lv_obj_set_pos(title, 12, 8);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF2F5F7), 0);
    s_wifi_label = lv_label_create(screen);
    lv_label_set_text(s_wifi_label, "Wi-Fi: ...");
    lv_obj_set_pos(s_wifi_label, 196, 8);
    lv_obj_set_width(s_wifi_label, 116);
    lv_obj_set_style_text_align(s_wifi_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0x95D5B2), 0);
    lv_obj_t *divider = lv_obj_create(screen);
    lv_obj_set_pos(divider, 8, 32);
    lv_obj_set_size(divider, 304, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x344451), 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);

    create_section_label(screen, "COLOR CHECK", 12, 37);
    create_swatch(screen, 12, "RED", lv_color_hex(0xE63946));
    create_swatch(screen, 88, "GREEN", lv_color_hex(0x2A9D58));
    create_swatch(screen, 164, "BLUE", lv_color_hex(0x2878D0));
    create_swatch(screen, 240, "WHITE", lv_color_white());

    lv_obj_t *camera_button = lv_button_create(screen);
    lv_obj_set_pos(camera_button, 12, 86);
    lv_obj_set_size(camera_button, 144, 39);
    lv_obj_add_event_cb(camera_button, camera_page_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *camera_button_label = lv_label_create(camera_button);
    lv_label_set_text(camera_button_label, "Camera Preview");
    lv_obj_center(camera_button_label);
    lv_obj_t *input_button = lv_button_create(screen);
    lv_obj_set_pos(input_button, 164, 86);
    lv_obj_set_size(input_button, 144, 39);
    lv_obj_add_event_cb(input_button, input_page_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *input_button_label = lv_label_create(input_button);
    lv_label_set_text(input_button_label, "Sensors / BOOT");
    lv_obj_center(input_button_label);

    create_section_label(screen, "DISPLAY", 12, 132);
    s_brightness_label = lv_label_create(screen);
    lv_label_set_text(s_brightness_label, "50%");
    lv_obj_set_pos(s_brightness_label, 220, 151);
    lv_obj_set_width(s_brightness_label, 32);
    lv_obj_set_style_text_color(s_brightness_label, lv_color_hex(0xDCE6ED), 0);
    lv_obj_t *slider = lv_slider_create(screen);
    lv_obj_set_pos(slider, 12, 150);
    lv_obj_set_size(slider, 196, 26);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *tap_button = lv_button_create(screen);
    lv_obj_set_pos(tap_button, 254, 147);
    lv_obj_set_size(tap_button, 54, 32);
    lv_obj_add_event_cb(tap_button, click_event_cb, LV_EVENT_CLICKED, NULL);
    s_counter_label = lv_label_create(tap_button);
    lv_label_set_text(s_counter_label, "Tap 0");
    lv_obj_center(s_counter_label);

    s_touch_fault_label = lv_label_create(screen);
    lv_label_set_text(s_touch_fault_label, "TOUCH OFFLINE");
    lv_obj_set_pos(s_touch_fault_label, 188, 185);
    lv_obj_set_style_text_color(s_touch_fault_label, lv_color_hex(0xF4A261), 0);
    lv_obj_add_flag(s_touch_fault_label, LV_OBJ_FLAG_HIDDEN);
    s_touch_label = lv_label_create(screen);
    lv_label_set_text(s_touch_label, "Touch R  x--- y---");
    lv_obj_set_pos(s_touch_label, 12, 187);
    lv_obj_set_width(s_touch_label, 170);
    lv_label_set_long_mode(s_touch_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_touch_label, lv_color_hex(0xD0D7DE), 0);
    s_runtime_label = lv_label_create(screen);
    lv_label_set_text(s_runtime_label, "Runtime  0s");
    lv_obj_set_pos(s_runtime_label, 12, 210);
    lv_obj_set_style_text_color(s_runtime_label, lv_color_hex(0x91A4B4), 0);

    const int32_t marker_x[] = {0, 314, 0, 314};
    const int32_t marker_y[] = {0, 0, 234, 234};
    for (size_t i = 0; i < 4; ++i) {
        lv_obj_t *marker = lv_obj_create(screen);
        lv_obj_set_pos(marker, marker_x[i], marker_y[i]);
        lv_obj_set_size(marker, 6, 6);
        lv_obj_set_style_bg_color(marker, lv_color_hex(0xFFD166), 0);
        lv_obj_set_style_border_width(marker, 0, 0);
        lv_obj_set_style_pad_all(marker, 0, 0);
    }

    s_camera_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_camera_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_camera_screen, LV_OPA_COVER, 0);
    s_camera_image = lv_image_create(s_camera_screen);
    lv_obj_set_pos(s_camera_image, 0, 0);
    lv_obj_set_size(s_camera_image, SZPI_CAMERA_WIDTH, SZPI_CAMERA_HEIGHT);
    lv_obj_add_flag(s_camera_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *controls = lv_obj_create(s_camera_screen);
    lv_obj_set_pos(controls, 0, 0);
    lv_obj_set_size(controls, SZPI_CAMERA_WIDTH, 34);
    lv_obj_set_style_bg_color(controls, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(controls, LV_OPA_70, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 2, 0);
    lv_obj_t *back = lv_button_create(controls);
    lv_obj_set_pos(back, 0, 0);
    lv_obj_set_size(back, 56, 28);
    lv_obj_add_event_cb(back, camera_back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_t *control = lv_button_create(controls);
    lv_obj_set_pos(control, 62, 0);
    lv_obj_set_size(control, 72, 28);
    lv_obj_add_event_cb(control, preview_control_event_cb, LV_EVENT_CLICKED, NULL);
    s_camera_control_label = lv_label_create(control);
    lv_label_set_text(s_camera_control_label, "Start");
    lv_obj_center(s_camera_control_label);
    s_camera_status_label = lv_label_create(controls);
    lv_label_set_text(s_camera_status_label, "Camera stopped");
    lv_obj_set_pos(s_camera_status_label, 140, 6);
    lv_obj_set_width(s_camera_status_label, 174);
    lv_label_set_long_mode(s_camera_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_camera_status_label, lv_color_white(), 0);
    s_camera_metrics_label = lv_label_create(s_camera_screen);
    lv_obj_set_pos(s_camera_metrics_label, 6, 198);
    lv_obj_set_size(s_camera_metrics_label, 308, 36);
    lv_obj_set_style_bg_color(s_camera_metrics_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_camera_metrics_label, LV_OPA_70, 0);
    lv_obj_set_style_text_color(s_camera_metrics_label, lv_color_white(), 0);

    s_input_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_input_screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(s_input_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_input_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *input_title = lv_label_create(s_input_screen);
    lv_label_set_text(input_title, "INPUT SENSORS");
    lv_obj_set_pos(input_title, 76, 13);
    lv_obj_set_style_text_color(input_title, lv_color_white(), 0);
    lv_obj_t *input_back = lv_button_create(s_input_screen);
    lv_obj_set_pos(input_back, 8, 6);
    lv_obj_set_size(input_back, 58, 30);
    lv_obj_add_event_cb(input_back, input_back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *input_back_label = lv_label_create(input_back);
    lv_label_set_text(input_back_label, "Back");
    lv_obj_center(input_back_label);
    lv_obj_t *input_retry = lv_button_create(s_input_screen);
    lv_obj_set_pos(input_retry, 250, 6);
    lv_obj_set_size(input_retry, 62, 30);
    lv_obj_add_event_cb(input_retry, input_retry_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *input_retry_label = lv_label_create(input_retry);
    lv_label_set_text(input_retry_label, "Retry");
    lv_obj_center(input_retry_label);
    s_input_identity_label = lv_label_create(s_input_screen);
    lv_obj_set_pos(s_input_identity_label, 14, 42);
    lv_obj_set_width(s_input_identity_label, 292);
    lv_label_set_long_mode(s_input_identity_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_input_identity_label, lv_color_hex(0x95D5B2), 0);

    lv_obj_t *accel_panel = create_panel(s_input_screen, 10, 62, 300, 44);
    s_input_accel_label = lv_label_create(accel_panel);
    lv_obj_set_pos(s_input_accel_label, 10, 5);
    lv_obj_set_width(s_input_accel_label, 280);
    lv_label_set_long_mode(s_input_accel_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_input_accel_label, lv_color_white(), 0);
    lv_obj_t *gyro_panel = create_panel(s_input_screen, 10, 111, 300, 44);
    s_input_gyro_label = lv_label_create(gyro_panel);
    lv_obj_set_pos(s_input_gyro_label, 10, 5);
    lv_obj_set_width(s_input_gyro_label, 280);
    lv_label_set_long_mode(s_input_gyro_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_input_gyro_label, lv_color_white(), 0);

    lv_obj_t *tilt_panel = create_panel(s_input_screen, 10, 160, 300, 40);
    s_input_tilt_label = lv_label_create(tilt_panel);
    lv_obj_set_pos(s_input_tilt_label, 10, 3);
    lv_obj_set_width(s_input_tilt_label, 280);
    lv_label_set_long_mode(s_input_tilt_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_input_tilt_label, lv_color_white(), 0);
    s_input_status_label = lv_label_create(tilt_panel);
    lv_obj_set_pos(s_input_status_label, 10, 20);
    lv_obj_set_width(s_input_status_label, 280);
    lv_label_set_long_mode(s_input_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_input_status_label, lv_color_hex(0xFFD166), 0);

    lv_obj_t *boot_panel = create_panel(s_input_screen, 10, 206, 300, 27);
    s_boot_status_label = lv_label_create(boot_panel);
    lv_obj_set_pos(s_boot_status_label, 10, 5);
    lv_obj_set_width(s_boot_status_label, 280);
    lv_label_set_long_mode(s_boot_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_boot_status_label, lv_color_hex(0xD0D7DE), 0);
}

static const char *wifi_state_name(szpi_wifi_service_state_t state)
{
    switch (state) {
    case SZPI_WIFI_DISABLED: return "disabled";
    case SZPI_WIFI_NO_CONFIG: return "no config";
    case SZPI_WIFI_STOPPED: return "stopped";
    case SZPI_WIFI_CONNECTING: return "connecting";
    case SZPI_WIFI_LINK_UP: return "link up";
    case SZPI_WIFI_ONLINE: return "online";
    case SZPI_WIFI_RETRY_WAIT: return "retry";
    case SZPI_WIFI_FAILED: return "failed";
    case SZPI_WIFI_STOPPING: return "stopping";
    default: return "unknown";
    }
}

static void update_page_status(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((uint32_t)(now - s_last_status_update) < 1000U) return;
    s_last_status_update = now;
    szpi_wifi_status_t wifi = {0};
    if (szpi_app_wifi_get_status(&wifi) == ESP_OK && s_wifi_label != NULL) {
        if (wifi.state == SZPI_WIFI_ONLINE) lv_label_set_text(s_wifi_label, "Wi-Fi: ONLINE");
        else lv_label_set_text_fmt(s_wifi_label, "Wi-Fi: %s", wifi_state_name(wifi.state));
    }
    uint32_t runtime_seconds = (uint32_t)((esp_timer_get_time() - s_started_at_us) / 1000000);
    if (s_runtime_label != NULL) lv_label_set_text_fmt(s_runtime_label, "Runtime  %" PRIu32 "s", runtime_seconds);
    if (s_touch_label != NULL) lv_label_set_text_fmt(s_touch_label, "Touch %c  x%u y%u",
        s_touch_pressed ? 'P' : 'R', (unsigned)s_touch_x, (unsigned)s_touch_y);
    if (s_touch_fault_label != NULL) {
        if (s_touch_faulted) lv_obj_remove_flag(s_touch_fault_label, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_touch_fault_label, LV_OBJ_FLAG_HIDDEN);
    }
    szpi_camera_preview_status_t preview = {0};
    if (szpi_app_camera_preview_get_status(&preview) == ESP_OK) {
        const char *state = "stopped";
        switch (preview.state) {
        case SZPI_CAMERA_PREVIEW_UNAVAILABLE: state = "unavailable"; break;
        case SZPI_CAMERA_PREVIEW_STARTING: state = "starting"; break;
        case SZPI_CAMERA_PREVIEW_RUNNING: state = "running"; break;
        case SZPI_CAMERA_PREVIEW_STOPPING: state = "stopping"; break;
        case SZPI_CAMERA_PREVIEW_FAULT: state = "fault"; break;
        default: break;
        }
        if (s_camera_status_label != NULL) {
            if (preview.state == SZPI_CAMERA_PREVIEW_RUNNING) {
                lv_label_set_text_fmt(s_camera_status_label, "LIVE %" PRIu32 ".%u fps",
                    preview.display_fps_milli / 1000U,
                    (unsigned)((preview.display_fps_milli % 1000U) / 100U));
            } else if (preview.last_error != ESP_OK) {
                lv_label_set_text_fmt(s_camera_status_label, "%s: %s", state, esp_err_to_name(preview.last_error));
            } else {
                lv_label_set_text_fmt(s_camera_status_label, "Camera %s", state);
            }
        }
        if (s_camera_control_label != NULL) {
            lv_label_set_text(s_camera_control_label,
                preview.state == SZPI_CAMERA_PREVIEW_RUNNING || preview.state == SZPI_CAMERA_PREVIEW_STARTING ||
                preview.state == SZPI_CAMERA_PREVIEW_STOPPING ? "Stop" : "Start");
        }
        if (s_camera_metrics_label != NULL) {
            lv_label_set_text_fmt(s_camera_metrics_label,
                "CAP %" PRIu32 "us  COPY %" PRIu32 "us\nLCD %" PRIu32 "us  ERR %" PRIu32,
                preview.max_capture_us, preview.max_copy_us, preview.max_refresh_us,
                preview.error_count);
        }
    }
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
    if (s_input_identity_label != NULL) {
        if (s_imu_ready) lv_label_set_text_fmt(s_input_identity_label,
            "QMI8658A WHO=0x%02x REV=0x%02x", s_imu_info.who_am_i, s_imu_info.revision);
        else lv_label_set_text(s_input_identity_label, "QMI8658A unavailable (Retry)");
    }
    if (s_input_accel_label != NULL && s_imu_sample.sequence != 0) {
        char accel_values[3][12];
        for (size_t axis = 0; axis < 3; ++axis) {
            format_fixed_value(accel_values[axis], sizeof(accel_values[axis]),
                s_imu_sample.accel_g[axis], 100, 2);
        }
        lv_label_set_text_fmt(s_input_accel_label,
            "A(g) X:%s Y:%s Z:%s\nRaw X:%+d Y:%+d Z:%+d",
            accel_values[0], accel_values[1], accel_values[2],
            (int)s_imu_sample.accel_raw[0], (int)s_imu_sample.accel_raw[1],
            (int)s_imu_sample.accel_raw[2]);
    } else if (s_input_accel_label != NULL) lv_label_set_text(s_input_accel_label, "ACCEL  waiting for sample");
    if (s_input_gyro_label != NULL && s_imu_sample.sequence != 0) {
        char gyro_values[3][12];
        for (size_t axis = 0; axis < 3; ++axis) {
            format_fixed_value(gyro_values[axis], sizeof(gyro_values[axis]),
                s_imu_sample.gyro_dps[axis], 10, 1);
        }
        lv_label_set_text_fmt(s_input_gyro_label,
            "G(dps) X:%s Y:%s Z:%s\nRaw X:%+d Y:%+d Z:%+d",
            gyro_values[0], gyro_values[1], gyro_values[2],
            (int)s_imu_sample.gyro_raw[0], (int)s_imu_sample.gyro_raw[1],
            (int)s_imu_sample.gyro_raw[2]);
    } else if (s_input_gyro_label != NULL) lv_label_set_text(s_input_gyro_label, "GYRO  waiting for sample");
    if (s_input_tilt_label != NULL) {
        if (s_imu_sample.tilt_reliable && s_imu_sample.valid) {
            char roll[12];
            char pitch[12];
            format_fixed_value(roll, sizeof(roll), s_imu_sample.roll_deg, 10, 1);
            format_fixed_value(pitch, sizeof(pitch), s_imu_sample.pitch_deg, 10, 1);
            lv_label_set_text_fmt(s_input_tilt_label, "Tilt R:%s deg  P:%s deg", roll, pitch);
        } else lv_label_set_text(s_input_tilt_label, "TILT   unreliable");
    }
    if (s_input_status_label != NULL) {
        const char *state = !s_imu_ready ? "FAULT" :
            !s_imu_sample.valid ? "WAIT" :
            s_imu_sample.fresh ? "FRESH" : "STALE";
        lv_label_set_text_fmt(s_input_status_label, "IMU %s  #%" PRIu32 "  %" PRIu32 "us",
            state, s_imu_sample.sequence, s_imu_sample.read_duration_us);
    }
    if (s_boot_status_label != NULL) {
        lv_label_set_text_fmt(s_boot_status_label, "BOOT %s  short %" PRIu32 "  long %" PRIu32,
            !s_boot_ready ? "OFFLINE" : s_boot_state.pressed ? "PRESSED" : "released",
            s_boot_short_count, s_boot_long_count);
    }
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
    if (!s_camera_page_active) {
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
    lv_image_cache_drop(&s_camera_image_descriptor);
    lv_obj_remove_flag(s_camera_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_camera_image);
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
    s_click_count = 0;
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
    s_brightness = 50;
    s_camera_page_active = false;
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
    make_ui();
    s_camera_staging = heap_caps_malloc(SZPI_CAMERA_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_camera_staging != NULL) {
        s_camera_image_descriptor = (lv_image_dsc_t){
            .header = {
                .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_RGB565,
                .w = SZPI_CAMERA_WIDTH,
                .h = SZPI_CAMERA_HEIGHT,
                .stride = SZPI_CAMERA_WIDTH * 2U,
            },
            .data_size = SZPI_CAMERA_FRAME_BYTES,
            .data = s_camera_staging,
        };
        lv_image_set_src(s_camera_image, &s_camera_image_descriptor);
    } else {
        ESP_LOGE(TAG, "camera preview disabled: cannot allocate %u-byte PSRAM staging buffer",
            (unsigned)SZPI_CAMERA_FRAME_BYTES);
    }
    s_started_at_us = esp_timer_get_time();
    lv_refr_now(s_lv_display);
    err = szpi_display_wait_flush(pdMS_TO_TICKS(1000));
    if (err != ESP_OK) return err;
    err = szpi_display_set_brightness(s_brightness);
    if (err != ESP_OK) return err;
    set_ui_state(s_touch_faulted ? SZPI_UI_TOUCH_FAULT : SZPI_UI_READY, ESP_OK);
    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_UI_READY);
    ESP_LOGI(TAG, "initial UI refresh completed; backlight=%u%%", (unsigned)s_brightness);
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
    if (s_camera_image != NULL) {
        lv_image_cache_drop(&s_camera_image_descriptor);
        lv_image_set_src(s_camera_image, NULL);
    }
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
    s_wifi_label = NULL;
    s_counter_label = NULL;
    s_brightness_label = NULL;
    s_touch_label = NULL;
    s_runtime_label = NULL;
    s_touch_fault_label = NULL;
    s_home_screen = NULL;
    s_camera_screen = NULL;
    s_camera_image = NULL;
    s_camera_status_label = NULL;
    s_camera_metrics_label = NULL;
    s_camera_control_label = NULL;
    s_input_screen = NULL;
    s_input_identity_label = NULL;
    s_input_accel_label = NULL;
    s_input_gyro_label = NULL;
    s_input_tilt_label = NULL;
    s_input_status_label = NULL;
    s_boot_status_label = NULL;
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
            update_page_status();
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
