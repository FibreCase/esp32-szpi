#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "szpi_display.h"
#include "szpi_audio.h"
#include "szpi_runtime_internal.h"
#include "ui_pages.h"

#define TAG "szpi_ui_pages"

static uint32_t s_click_count;
static uint32_t s_last_status_update;
static uint8_t s_brightness = 50;
static bool s_camera_page_active;
static bool s_camera_available;
static bool s_pages_created;
static bool s_storage_ready;
static bool s_storage_format_armed;
static uint32_t s_storage_confirm_generation;
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
static lv_obj_t *s_audio_screen;
static lv_obj_t *s_storage_screen;
static lv_obj_t *s_storage_status_label;
static lv_obj_t *s_audio_status_label;
static lv_obj_t *s_audio_input_gain_label;
static lv_obj_t *s_audio_volume_label;
static lv_obj_t *s_record_button;
static lv_obj_t *s_play_button;
static lv_obj_t *s_storage_confirm_button;
static lv_obj_t *s_storage_confirm_label;
static lv_obj_t *s_input_identity_label;
static lv_obj_t *s_input_accel_label;
static lv_obj_t *s_input_gyro_label;
static lv_obj_t *s_input_tilt_label;
static lv_obj_t *s_input_status_label;
static lv_obj_t *s_boot_status_label;
static lv_image_dsc_t s_camera_image_descriptor;

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
    if (!s_camera_available) return;
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

static void storage_page_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    s_storage_format_armed = false;
    lv_obj_add_state(s_storage_confirm_button, LV_STATE_DISABLED);
    lv_label_set_text(s_storage_confirm_label, "Erase SD");
    lv_screen_load(s_storage_screen);
}

static void audio_page_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_screen_load(s_audio_screen);
}

static void audio_back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_screen_load(s_home_screen);
}

static void storage_back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_screen_load(s_audio_screen);
}

static void storage_retry_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    esp_err_t err = szpi_app_storage_retry();
    if (err != ESP_OK) ESP_LOGW(TAG, "storage retry queue failed: %s", esp_err_to_name(err));
}

static void storage_arm_format_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    szpi_storage_status_t status = {0};
    if (szpi_app_storage_get_status(&status) != ESP_OK || status.state == SZPI_STORAGE_NO_CARD ||
        status.state == SZPI_STORAGE_UNINITIALIZED || status.capacity_bytes == 0) return;
    s_storage_confirm_generation = status.generation;
    s_storage_format_armed = true;
    lv_obj_clear_state(s_storage_confirm_button, LV_STATE_DISABLED);
    lv_label_set_text(s_storage_confirm_label, "ERASE SD");
}

static void storage_confirm_format_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !s_storage_format_armed) return;
    szpi_storage_status_t status = {0};
    if (szpi_app_storage_get_status(&status) != ESP_OK || status.generation != s_storage_confirm_generation) {
        s_storage_format_armed = false;
        lv_obj_add_state(s_storage_confirm_button, LV_STATE_DISABLED);
        lv_label_set_text(s_storage_confirm_label, "Erase SD");
        return;
    }
    esp_err_t err = szpi_app_storage_format_confirmed(s_storage_confirm_generation);
    s_storage_format_armed = false;
    lv_obj_add_state(s_storage_confirm_button, LV_STATE_DISABLED);
    lv_label_set_text(s_storage_confirm_label, "Erase SD");
    if (err != ESP_OK) ESP_LOGW(TAG, "format request queue failed: %s", esp_err_to_name(err));
}

static void audio_tone_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    esp_err_t err = szpi_app_audio_test_tone();
    if (err != ESP_OK) ESP_LOGW(TAG, "audio test tone request failed: %s", esp_err_to_name(err));
}

static void audio_capture_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    esp_err_t err = szpi_app_audio_capture_test();
    if (err != ESP_OK) ESP_LOGW(TAG, "audio capture test request failed: %s", esp_err_to_name(err));
}

static void audio_record_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    szpi_storage_status_t storage = {0};
    if (szpi_app_storage_get_status(&storage) != ESP_OK || storage.state != SZPI_STORAGE_READY) {
        ESP_LOGW(TAG, "record unavailable until SD is FAT32 ready");
        return;
    }
    esp_err_t err = szpi_app_audio_record_start();
    if (err != ESP_OK) ESP_LOGW(TAG, "record request failed: %s", esp_err_to_name(err));
}

static void audio_play_latest_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    esp_err_t err = szpi_app_audio_play_latest();
    if (err != ESP_OK) ESP_LOGW(TAG, "playback request failed: %s", esp_err_to_name(err));
}

static void audio_stop_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    esp_err_t err = szpi_app_audio_stop();
    if (err != ESP_OK) ESP_LOGW(TAG, "audio stop request failed: %s", esp_err_to_name(err));
}

static void input_retry_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    szpi_ui_service_retry_inputs();
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

static void audio_volume_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    int32_t value = lv_slider_get_value(lv_event_get_target(event));
    if (value < 0 || value > SZPI_AUDIO_MAX_VOLUME_PERCENT) return;
    if (s_audio_volume_label != NULL) {
        lv_label_set_text_fmt(s_audio_volume_label, "Speaker volume  %d%%", (int)value);
    }
    esp_err_t err = szpi_app_audio_set_volume((uint8_t)value);
    if (err != ESP_OK) ESP_LOGW(TAG, "speaker volume request failed: %s", esp_err_to_name(err));
}

static void audio_input_gain_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    int32_t value = lv_slider_get_value(lv_event_get_target(event));
    if (value < 0 || value > SZPI_AUDIO_MAX_INPUT_GAIN_DB) return;
    if (s_audio_input_gain_label != NULL) {
        lv_label_set_text_fmt(s_audio_input_gain_label, "Microphone gain  %d dB", (int)value);
    }
    esp_err_t err = szpi_app_audio_set_input_gain((uint8_t)value);
    if (err != ESP_OK) ESP_LOGW(TAG, "microphone gain request failed: %s", esp_err_to_name(err));
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
    lv_obj_set_size(camera_button, 94, 39);
    lv_obj_add_event_cb(camera_button, camera_page_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *camera_button_label = lv_label_create(camera_button);
    lv_label_set_text(camera_button_label, "Camera");
    lv_obj_center(camera_button_label);
    lv_obj_t *input_button = lv_button_create(screen);
    lv_obj_set_pos(input_button, 111, 86);
    lv_obj_set_size(input_button, 94, 39);
    lv_obj_add_event_cb(input_button, input_page_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *input_button_label = lv_label_create(input_button);
    lv_label_set_text(input_button_label, "Sensors");
    lv_obj_center(input_button_label);
    lv_obj_t *storage_button = lv_button_create(screen);
    lv_obj_set_pos(storage_button, 210, 86);
    lv_obj_set_size(storage_button, 98, 39);
    lv_obj_add_event_cb(storage_button, audio_page_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *storage_button_label = lv_label_create(storage_button);
    lv_label_set_text(storage_button_label, "Audio");
    lv_obj_center(storage_button_label);

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

    s_audio_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_audio_screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(s_audio_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_audio_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *audio_title = lv_label_create(s_audio_screen);
    lv_label_set_text(audio_title, "AUDIO");
    lv_obj_set_pos(audio_title, 126, 13);
    lv_obj_set_style_text_color(audio_title, lv_color_white(), 0);
    lv_obj_t *audio_back = lv_button_create(s_audio_screen);
    lv_obj_set_pos(audio_back, 8, 6);
    lv_obj_set_size(audio_back, 58, 30);
    lv_obj_add_event_cb(audio_back, audio_back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *audio_back_label = lv_label_create(audio_back);
    lv_label_set_text(audio_back_label, "Back");
    lv_obj_center(audio_back_label);
    lv_obj_t *sd_button = lv_button_create(s_audio_screen);
    lv_obj_set_pos(sd_button, 236, 6);
    lv_obj_set_size(sd_button, 76, 30);
    lv_obj_add_event_cb(sd_button, storage_page_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sd_button_label = lv_label_create(sd_button);
    lv_label_set_text(sd_button_label, "SD Card");
    lv_obj_center(sd_button_label);
    lv_obj_t *tone_button = lv_button_create(s_audio_screen);
    lv_obj_set_pos(tone_button, 12, 46);
    lv_obj_set_size(tone_button, 94, 27);
    lv_obj_add_event_cb(tone_button, audio_tone_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tone_label = lv_label_create(tone_button);
    lv_label_set_text(tone_label, "Test tone");
    lv_obj_center(tone_label);
    lv_obj_t *mic_button = lv_button_create(s_audio_screen);
    lv_obj_set_pos(mic_button, 111, 46);
    lv_obj_set_size(mic_button, 94, 27);
    lv_obj_add_event_cb(mic_button, audio_capture_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mic_label = lv_label_create(mic_button);
    lv_label_set_text(mic_label, "Mic level");
    lv_obj_center(mic_label);
    lv_obj_t *audio_stop = lv_button_create(s_audio_screen);
    lv_obj_set_pos(audio_stop, 210, 46);
    lv_obj_set_size(audio_stop, 98, 27);
    lv_obj_add_event_cb(audio_stop, audio_stop_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *audio_stop_label = lv_label_create(audio_stop);
    lv_label_set_text(audio_stop_label, "Audio stop");
    lv_obj_center(audio_stop_label);
    s_audio_input_gain_label = lv_label_create(s_audio_screen);
    lv_label_set_text_fmt(s_audio_input_gain_label, "Microphone gain  %u dB", SZPI_AUDIO_DEFAULT_INPUT_GAIN_DB);
    lv_obj_set_pos(s_audio_input_gain_label, 14, 80);
    lv_obj_set_style_text_color(s_audio_input_gain_label, lv_color_hex(0xDCE6ED), 0);
    lv_obj_t *audio_input_gain_slider = lv_slider_create(s_audio_screen);
    lv_obj_set_pos(audio_input_gain_slider, 16, 101);
    lv_obj_set_size(audio_input_gain_slider, 288, 20);
    lv_slider_set_range(audio_input_gain_slider, 0, SZPI_AUDIO_MAX_INPUT_GAIN_DB);
    lv_slider_set_value(audio_input_gain_slider, SZPI_AUDIO_DEFAULT_INPUT_GAIN_DB, LV_ANIM_OFF);
    lv_obj_add_event_cb(audio_input_gain_slider, audio_input_gain_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_audio_volume_label = lv_label_create(s_audio_screen);
    lv_label_set_text_fmt(s_audio_volume_label, "Speaker volume  %u%%", SZPI_AUDIO_DEFAULT_VOLUME_PERCENT);
    lv_obj_set_pos(s_audio_volume_label, 14, 125);
    lv_obj_set_style_text_color(s_audio_volume_label, lv_color_hex(0xDCE6ED), 0);
    lv_obj_t *audio_volume_slider = lv_slider_create(s_audio_screen);
    lv_obj_set_pos(audio_volume_slider, 16, 146);
    lv_obj_set_size(audio_volume_slider, 288, 20);
    lv_slider_set_range(audio_volume_slider, 0, SZPI_AUDIO_MAX_VOLUME_PERCENT);
    lv_slider_set_value(audio_volume_slider, SZPI_AUDIO_DEFAULT_VOLUME_PERCENT, LV_ANIM_OFF);
    lv_obj_add_event_cb(audio_volume_slider, audio_volume_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_record_button = lv_button_create(s_audio_screen);
    lv_obj_set_pos(s_record_button, 12, 174);
    lv_obj_set_size(s_record_button, 144, 27);
    lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_record_button, audio_record_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *record_label = lv_label_create(s_record_button);
    lv_label_set_text(record_label, "Record WAV");
    lv_obj_center(record_label);
    s_play_button = lv_button_create(s_audio_screen);
    lv_obj_set_pos(s_play_button, 164, 174);
    lv_obj_set_size(s_play_button, 144, 27);
    lv_obj_add_state(s_play_button, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_play_button, audio_play_latest_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *play_label = lv_label_create(s_play_button);
    lv_label_set_text(play_label, "Play latest WAV");
    lv_obj_center(play_label);
    s_audio_status_label = lv_label_create(s_audio_screen);
    lv_obj_set_pos(s_audio_status_label, 14, 211);
    lv_obj_set_size(s_audio_status_label, 292, 20);
    lv_label_set_long_mode(s_audio_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_audio_status_label, lv_color_hex(0x95D5B2), 0);
    lv_label_set_text(s_audio_status_label, "Audio tests: idle");

    s_storage_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_storage_screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(s_storage_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_storage_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *storage_title = lv_label_create(s_storage_screen);
    lv_label_set_text(storage_title, "SD CARD");
    lv_obj_set_pos(storage_title, 119, 13);
    lv_obj_set_style_text_color(storage_title, lv_color_white(), 0);
    lv_obj_t *storage_back = lv_button_create(s_storage_screen);
    lv_obj_set_pos(storage_back, 8, 6);
    lv_obj_set_size(storage_back, 58, 30);
    lv_obj_add_event_cb(storage_back, storage_back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *storage_back_label = lv_label_create(storage_back);
    lv_label_set_text(storage_back_label, "Back");
    lv_obj_center(storage_back_label);
    lv_obj_t *storage_retry = lv_button_create(s_storage_screen);
    lv_obj_set_pos(storage_retry, 250, 6);
    lv_obj_set_size(storage_retry, 62, 30);
    lv_obj_add_event_cb(storage_retry, storage_retry_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *storage_retry_label = lv_label_create(storage_retry);
    lv_label_set_text(storage_retry_label, "Retry");
    lv_obj_center(storage_retry_label);
    s_storage_status_label = lv_label_create(s_storage_screen);
    lv_obj_set_pos(s_storage_status_label, 14, 48);
    lv_obj_set_size(s_storage_status_label, 292, 66);
    lv_label_set_long_mode(s_storage_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_storage_status_label, lv_color_hex(0xD0D7DE), 0);
    lv_label_set_text(s_storage_status_label, "Initializing SD card...");
    lv_obj_t *format_warning = lv_label_create(s_storage_screen);
    lv_obj_set_pos(format_warning, 14, 127);
    lv_obj_set_size(format_warning, 292, 36);
    lv_label_set_text(format_warning, "Format FAT32 erases the ENTIRE SD card.");
    lv_label_set_long_mode(format_warning, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(format_warning, lv_color_hex(0xF4A261), 0);
    lv_obj_t *format_arm = lv_button_create(s_storage_screen);
    lv_obj_set_pos(format_arm, 18, 185);
    lv_obj_set_size(format_arm, 132, 36);
    lv_obj_add_event_cb(format_arm, storage_arm_format_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *format_arm_label = lv_label_create(format_arm);
    lv_label_set_text(format_arm_label, "Prepare format");
    lv_obj_center(format_arm_label);
    s_storage_confirm_button = lv_button_create(s_storage_screen);
    lv_obj_set_pos(s_storage_confirm_button, 166, 185);
    lv_obj_set_size(s_storage_confirm_button, 132, 36);
    lv_obj_add_state(s_storage_confirm_button, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_storage_confirm_button, storage_confirm_format_event_cb, LV_EVENT_CLICKED, NULL);
    s_storage_confirm_label = lv_label_create(s_storage_confirm_button);
    lv_label_set_text(s_storage_confirm_label, "Erase SD");
    lv_obj_center(s_storage_confirm_label);
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

void szpi_ui_pages_update_status(uint32_t runtime_seconds, bool touch_pressed,
                                 uint16_t touch_x, uint16_t touch_y, bool touch_faulted)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((uint32_t)(now - s_last_status_update) < 1000U) return;
    s_last_status_update = now;
    szpi_wifi_status_t wifi = {0};
    if (szpi_app_wifi_get_status(&wifi) == ESP_OK && s_wifi_label != NULL) {
        if (wifi.state == SZPI_WIFI_ONLINE) lv_label_set_text(s_wifi_label, "Wi-Fi: ONLINE");
        else lv_label_set_text_fmt(s_wifi_label, "Wi-Fi: %s", wifi_state_name(wifi.state));
    }
    if (s_runtime_label != NULL) lv_label_set_text_fmt(s_runtime_label, "Runtime  %" PRIu32 "s", runtime_seconds);
    if (s_touch_label != NULL) lv_label_set_text_fmt(s_touch_label, "Touch %c  x%u y%u",
        touch_pressed ? 'P' : 'R', (unsigned)touch_x, (unsigned)touch_y);
    if (s_touch_fault_label != NULL) {
        if (touch_faulted) lv_obj_remove_flag(s_touch_fault_label, LV_OBJ_FLAG_HIDDEN);
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
    szpi_storage_status_t storage = {0};
    if (s_storage_status_label != NULL && szpi_app_storage_get_status(&storage) == ESP_OK) {
        s_storage_ready = storage.state == SZPI_STORAGE_READY;
        if (s_record_button != NULL) {
            if (s_storage_ready) lv_obj_clear_state(s_record_button, LV_STATE_DISABLED);
            else lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
        }
        const char *state = "unknown";
        switch (storage.state) {
        case SZPI_STORAGE_UNINITIALIZED: state = "starting"; break;
        case SZPI_STORAGE_NO_CARD: state = "no card"; break;
        case SZPI_STORAGE_CARD_READY_NO_FS: state = "card ready / no FAT32"; break;
        case SZPI_STORAGE_READY: state = "FAT32 ready"; break;
        case SZPI_STORAGE_BUSY: state = "busy"; break;
        case SZPI_STORAGE_FORMATTING: state = "formatting"; break;
        case SZPI_STORAGE_FAULT: state = "fault"; break;
        }
        lv_label_set_text_fmt(s_storage_status_label,
            "SD: %s\nCapacity: %llu MiB\nFree: %llu MiB\nAudio: ES7210 / ES8311 hardware",
            state, (unsigned long long)(storage.capacity_bytes / (1024U * 1024U)),
            (unsigned long long)(storage.free_bytes / (1024U * 1024U)));
        if (s_storage_format_armed && storage.generation != s_storage_confirm_generation) {
            s_storage_format_armed = false;
            lv_obj_add_state(s_storage_confirm_button, LV_STATE_DISABLED);
            lv_label_set_text(s_storage_confirm_label, "Erase SD");
        }
    }
    szpi_audio_service_status_t audio = {0};
    if (s_audio_status_label != NULL && szpi_app_audio_get_status(&audio) == ESP_OK) {
        bool audio_active = audio.state == SZPI_AUDIO_SERVICE_PLAYING_TEST ||
            audio.state == SZPI_AUDIO_SERVICE_CAPTURE_TEST || audio.state == SZPI_AUDIO_SERVICE_RECORDING ||
            audio.state == SZPI_AUDIO_SERVICE_PLAYING_FILE;
        bool has_last_wav = strlen(audio.last_file) >= 4 &&
            strcmp(audio.last_file + strlen(audio.last_file) - 4, ".wav") == 0;
        if (s_record_button != NULL) {
            if (s_storage_ready && !audio_active) lv_obj_clear_state(s_record_button, LV_STATE_DISABLED);
            else lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
        }
        if (s_play_button != NULL) {
            if (s_storage_ready && has_last_wav && !audio_active) lv_obj_clear_state(s_play_button, LV_STATE_DISABLED);
            else lv_obj_add_state(s_play_button, LV_STATE_DISABLED);
        }
        const char *state = "offline";
        switch (audio.state) {
        case SZPI_AUDIO_SERVICE_IDLE: state = "idle"; break;
        case SZPI_AUDIO_SERVICE_PLAYING_TEST: state = "test tone"; break;
        case SZPI_AUDIO_SERVICE_CAPTURE_TEST: state = "mic capture"; break;
        case SZPI_AUDIO_SERVICE_RECORDING: state = "recording"; break;
        case SZPI_AUDIO_SERVICE_PLAYING_FILE: state = "playing WAV"; break;
        case SZPI_AUDIO_SERVICE_COMPLETE: state = "complete"; break;
        case SZPI_AUDIO_SERVICE_FAULT: state = "fault"; break;
        default: break;
        }
        lv_label_set_text_fmt(s_audio_status_label, "Audio %s blocks:%" PRIu32 " peak:%u rms:%u",
            state, audio.blocks_processed, (unsigned)audio.peak_sample, (unsigned)audio.rms_sample);
    }
}


void szpi_ui_pages_create(void)
{
    s_click_count = 0;
    s_last_status_update = 0;
    s_brightness = 50;
    s_camera_page_active = false;
    s_camera_available = false;
    s_storage_ready = false;
    s_storage_format_armed = false;
    s_storage_confirm_generation = 0;
    s_camera_image_descriptor = (lv_image_dsc_t){0};
    make_ui();
    s_pages_created = true;
}

void szpi_ui_pages_destroy(void)
{
    if (s_pages_created) szpi_ui_pages_clear_camera_source();
    s_pages_created = false;
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
    s_audio_screen = NULL;
    s_storage_screen = NULL;
    s_storage_status_label = NULL;
    s_audio_status_label = NULL;
    s_audio_input_gain_label = NULL;
    s_audio_volume_label = NULL;
    s_record_button = NULL;
    s_play_button = NULL;
    s_storage_confirm_button = NULL;
    s_storage_confirm_label = NULL;
    s_input_identity_label = NULL;
    s_input_accel_label = NULL;
    s_input_gyro_label = NULL;
    s_input_tilt_label = NULL;
    s_input_status_label = NULL;
    s_boot_status_label = NULL;
    s_camera_image_descriptor = (lv_image_dsc_t){0};
    s_camera_page_active = false;
    s_camera_available = false;
}

bool szpi_ui_pages_camera_active(void)
{
    return s_camera_page_active;
}

void szpi_ui_pages_set_camera_source(uint8_t *data)
{
    if (s_camera_image == NULL || data == NULL) return;
    s_camera_image_descriptor = (lv_image_dsc_t){
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RGB565,
            .w = SZPI_CAMERA_WIDTH,
            .h = SZPI_CAMERA_HEIGHT,
            .stride = SZPI_CAMERA_WIDTH * 2U,
        },
        .data_size = SZPI_CAMERA_FRAME_BYTES,
        .data = data,
    };
    lv_image_set_src(s_camera_image, &s_camera_image_descriptor);
    s_camera_available = true;
}

void szpi_ui_pages_clear_camera_source(void)
{
    if (s_camera_image != NULL) {
        lv_image_cache_drop(&s_camera_image_descriptor);
        lv_image_set_src(s_camera_image, NULL);
    }
    s_camera_available = false;
}

void szpi_ui_pages_show_camera_frame(void)
{
    if (s_camera_image == NULL) return;
    lv_image_cache_drop(&s_camera_image_descriptor);
    lv_obj_remove_flag(s_camera_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_camera_image);
}

void szpi_ui_pages_show_home(void)
{
    s_camera_page_active = false;
    if (s_home_screen != NULL) lv_screen_load(s_home_screen);
}

uint8_t szpi_ui_pages_brightness(void)
{
    return s_brightness;
}

void szpi_ui_pages_update_inputs(bool imu_ready, const szpi_imu_info_t *imu_info,
                                 const szpi_imu_sample_t *imu_sample, bool boot_ready,
                                 const szpi_boot_state_t *boot_state,
                                 uint32_t boot_short_count, uint32_t boot_long_count)
{
    if (imu_info == NULL || imu_sample == NULL || boot_state == NULL) return;
    if (s_input_identity_label != NULL) {
        if (imu_ready) lv_label_set_text_fmt(s_input_identity_label,
            "QMI8658A WHO=0x%02x REV=0x%02x", imu_info->who_am_i, imu_info->revision);
        else lv_label_set_text(s_input_identity_label, "QMI8658A unavailable (Retry)");
    }
    if (s_input_accel_label != NULL && imu_sample->sequence != 0) {
        char accel_values[3][12];
        for (size_t axis = 0; axis < 3; ++axis) {
            format_fixed_value(accel_values[axis], sizeof(accel_values[axis]),
                imu_sample->accel_g[axis], 100, 2);
        }
        lv_label_set_text_fmt(s_input_accel_label,
            "A(g) X:%s Y:%s Z:%s\nRaw X:%+d Y:%+d Z:%+d",
            accel_values[0], accel_values[1], accel_values[2],
            (int)imu_sample->accel_raw[0], (int)imu_sample->accel_raw[1], (int)imu_sample->accel_raw[2]);
    } else if (s_input_accel_label != NULL) lv_label_set_text(s_input_accel_label, "ACCEL  waiting for sample");
    if (s_input_gyro_label != NULL && imu_sample->sequence != 0) {
        char gyro_values[3][12];
        for (size_t axis = 0; axis < 3; ++axis) {
            format_fixed_value(gyro_values[axis], sizeof(gyro_values[axis]),
                imu_sample->gyro_dps[axis], 10, 1);
        }
        lv_label_set_text_fmt(s_input_gyro_label,
            "G(dps) X:%s Y:%s Z:%s\nRaw X:%+d Y:%+d Z:%+d",
            gyro_values[0], gyro_values[1], gyro_values[2],
            (int)imu_sample->gyro_raw[0], (int)imu_sample->gyro_raw[1], (int)imu_sample->gyro_raw[2]);
    } else if (s_input_gyro_label != NULL) lv_label_set_text(s_input_gyro_label, "GYRO  waiting for sample");
    if (s_input_tilt_label != NULL) {
        if (imu_sample->tilt_reliable && imu_sample->valid) {
            char roll[12];
            char pitch[12];
            format_fixed_value(roll, sizeof(roll), imu_sample->roll_deg, 10, 1);
            format_fixed_value(pitch, sizeof(pitch), imu_sample->pitch_deg, 10, 1);
            lv_label_set_text_fmt(s_input_tilt_label, "Tilt R:%s deg  P:%s deg", roll, pitch);
        } else lv_label_set_text(s_input_tilt_label, "TILT   unreliable");
    }
    if (s_input_status_label != NULL) {
        const char *state = !imu_ready ? "FAULT" :
            !imu_sample->valid ? "WAIT" : imu_sample->fresh ? "FRESH" : "STALE";
        lv_label_set_text_fmt(s_input_status_label, "IMU %s  #%" PRIu32 "  %" PRIu32 "us",
            state, imu_sample->sequence, imu_sample->read_duration_us);
    }
    if (s_boot_status_label != NULL) {
        lv_label_set_text_fmt(s_boot_status_label, "BOOT %s  short %" PRIu32 "  long %" PRIu32,
            !boot_ready ? "OFFLINE" : boot_state->pressed ? "PRESSED" : "released",
            boot_short_count, boot_long_count);
    }
}
