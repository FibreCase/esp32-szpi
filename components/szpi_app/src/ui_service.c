#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "szpi_display.h"
#include "szpi_input.h"
#include "szpi_runtime_internal.h"

#define TAG "szpi_ui"
#define TOUCH_PERIOD_MS 20
#define UI_LOOP_PERIOD_MS 10
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
static int64_t s_started_at_us;
static uint8_t s_brightness = 50;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_counter_label;
static lv_obj_t *s_brightness_label;
static lv_obj_t *s_touch_label;
static lv_obj_t *s_runtime_label;
static lv_obj_t *s_touch_fault_label;

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
    if (s_counter_label != NULL) lv_label_set_text_fmt(s_counter_label, "Taps: %" PRIu32, s_click_count);
    if (szpi_ui_status_lock != NULL && xSemaphoreTake(szpi_ui_status_lock, 0) == pdTRUE) {
        szpi_ui_status.click_count = s_click_count;
        xSemaphoreGive(szpi_ui_status_lock);
    }
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
        lv_label_set_text_fmt(s_brightness_label, "Brightness: %" PRId32 "%%", value);
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
    lv_obj_set_pos(swatch, x, 38);
    lv_obj_set_size(swatch, 68, 28);
    lv_obj_set_style_bg_color(swatch, color, 0);
    lv_obj_set_style_border_width(swatch, 1, 0);
    lv_obj_set_style_border_color(swatch, lv_color_hex(0x8B9AAA), 0);
    lv_obj_set_style_pad_all(swatch, 0, 0);
    lv_obj_t *label = lv_label_create(swatch);
    lv_label_set_text(label, name);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    return swatch;
}

static void make_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "SZPI Display Test");
    lv_obj_set_pos(title, 8, 5);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF2F5F7), 0);
    s_wifi_label = lv_label_create(screen);
    lv_label_set_text(s_wifi_label, "Wi-Fi: ...");
    lv_obj_set_pos(s_wifi_label, 205, 6);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0x95D5B2), 0);

    create_swatch(screen, 8, "RED", lv_color_hex(0xE63946));
    create_swatch(screen, 84, "GREEN", lv_color_hex(0x2A9D58));
    create_swatch(screen, 160, "BLUE", lv_color_hex(0x2878D0));
    create_swatch(screen, 236, "WHITE", lv_color_white());

    lv_obj_t *tap_button = lv_button_create(screen);
    lv_obj_set_pos(tap_button, 12, 78);
    lv_obj_set_size(tap_button, 104, 40);
    lv_obj_add_event_cb(tap_button, click_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tap_label = lv_label_create(tap_button);
    lv_label_set_text(tap_label, "Tap +1");
    lv_obj_center(tap_label);
    s_counter_label = lv_label_create(screen);
    lv_label_set_text(s_counter_label, "Taps: 0");
    lv_obj_set_pos(s_counter_label, 132, 89);
    lv_obj_set_style_text_color(s_counter_label, lv_color_white(), 0);

    s_brightness_label = lv_label_create(screen);
    lv_label_set_text(s_brightness_label, "Brightness: 50%");
    lv_obj_set_pos(s_brightness_label, 12, 129);
    lv_obj_set_style_text_color(s_brightness_label, lv_color_white(), 0);
    lv_obj_t *slider = lv_slider_create(screen);
    lv_obj_set_pos(slider, 12, 151);
    lv_obj_set_size(slider, 296, 36);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_touch_fault_label = lv_label_create(screen);
    lv_label_set_text(s_touch_fault_label, "Touch unavailable");
    lv_obj_set_pos(s_touch_fault_label, 178, 130);
    lv_obj_set_style_text_color(s_touch_fault_label, lv_color_hex(0xF4A261), 0);
    lv_obj_set_hidden(s_touch_fault_label, true);

    s_touch_label = lv_label_create(screen);
    lv_label_set_text(s_touch_label, "Touch: Released  x=--- y=---");
    lv_obj_set_pos(s_touch_label, 8, 196);
    lv_obj_set_style_text_color(s_touch_label, lv_color_hex(0xD0D7DE), 0);
    s_runtime_label = lv_label_create(screen);
    lv_label_set_text(s_runtime_label, "Runtime: 0s");
    lv_obj_set_pos(s_runtime_label, 8, 218);
    lv_obj_set_style_text_color(s_runtime_label, lv_color_hex(0xD0D7DE), 0);

    const char *corners[] = {"TL", "TR", "BL", "BR"};
    const int32_t corner_x[] = {1, 296, 1, 296};
    const int32_t corner_y[] = {24, 24, 228, 228};
    for (size_t i = 0; i < 4; ++i) {
        lv_obj_t *marker = lv_label_create(screen);
        lv_label_set_text(marker, corners[i]);
        lv_obj_set_pos(marker, corner_x[i], corner_y[i]);
        lv_obj_set_style_text_color(marker, lv_color_hex(0xFFD166), 0);
    }
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
        if (wifi.state == SZPI_WIFI_ONLINE) lv_label_set_text_fmt(s_wifi_label, "Wi-Fi: online " IPSTR, IP2STR(&wifi.ip_info.ip));
        else lv_label_set_text_fmt(s_wifi_label, "Wi-Fi: %s", wifi_state_name(wifi.state));
    }
    uint32_t runtime_seconds = (uint32_t)((esp_timer_get_time() - s_started_at_us) / 1000000);
    if (s_runtime_label != NULL) lv_label_set_text_fmt(s_runtime_label, "Runtime: %" PRIu32 "s", runtime_seconds);
    if (s_touch_label != NULL) lv_label_set_text_fmt(s_touch_label, "Touch: %s  x=%u y=%u",
        s_touch_pressed ? "Pressed" : "Released", (unsigned)s_touch_x, (unsigned)s_touch_y);
    if (s_touch_fault_label != NULL) {
        lv_obj_set_hidden(s_touch_fault_label, !s_touch_faulted);
    }
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
    s_brightness = 50;
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
    make_ui();
    s_started_at_us = esp_timer_get_time();
    lv_refr_now(s_lv_display);
    err = szpi_display_wait_flush(pdMS_TO_TICKS(1000));
    if (err != ESP_OK) return err;
    err = szpi_display_set_brightness(s_brightness);
    if (err != ESP_OK) return err;
    set_ui_state(s_touch_faulted ? SZPI_UI_TOUCH_FAULT : SZPI_UI_READY, ESP_OK);
    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_UI_READY);
    return ESP_OK;
}

static void cleanup_ui(void)
{
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
    if (s_lv_input != NULL) { lv_indev_delete(s_lv_input); s_lv_input = NULL; }
    esp_err_t err = szpi_display_set_brightness(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) ESP_LOGW(TAG, "backlight off failed: %s", esp_err_to_name(err));
    err = szpi_display_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "display deinit failed: %s", esp_err_to_name(err));
    s_lv_display = NULL;
    if (s_lvgl_initialized) {
        lv_deinit();
        s_lvgl_initialized = false;
    }
    s_wifi_label = NULL;
    s_counter_label = NULL;
    s_brightness_label = NULL;
    s_touch_label = NULL;
    s_runtime_label = NULL;
    s_touch_fault_label = NULL;
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
            stop_requested = process_ui_command(pdMS_TO_TICKS(UI_LOOP_PERIOD_MS), &command);
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
        cleanup_ui();
        if (stop_requested) set_ui_state(SZPI_UI_STOPPED, ESP_OK);
        signal_stopped();
    }
}
