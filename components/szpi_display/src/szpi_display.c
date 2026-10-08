#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "szpi_board.h"
#include "szpi_display.h"
#include "src/draw/sw/lv_draw_sw_utils.h"

#define TAG "szpi_display"
#define LCD_QUEUE_DEPTH 1
#define FLUSH_WAIT_SLICE_MS 250

static const szpi_board_bindings_t *s_bindings;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_display;
static uint8_t *s_draw_buffer_1;
static uint8_t *s_draw_buffer_2;
static StaticSemaphore_t s_flush_semaphore_storage;
static SemaphoreHandle_t s_flush_semaphore;
static bool s_spi_initialized;
static bool s_ledc_timer_initialized;
static bool s_ledc_channel_initialized;
static bool s_cs_selected;
static bool s_initialized;
static bool s_orientation_inverted;
static bool s_flush_inflight;
static uint32_t s_flush_timeouts;
static esp_err_t s_last_flush_error;

/* ISR writes completion time before publishing the existing DMA semaphore.
 * Only the UI task reads it, after taking that semaphore. */
static volatile int64_t s_dma_done_us;
static bool s_test_active;
static bool s_test_frame_active;
static bool s_test_flush_tracked;
static bool s_test_flush_last;
static int64_t s_test_window_us;
static int64_t s_test_frame_start_us;
static int64_t s_test_last_dma_us;
static int64_t s_test_render_start_us;
static uint32_t s_test_frames;
static uint32_t s_test_renders;
static uint32_t s_test_pixels;
static uint32_t s_test_frame_pixels;
static uint64_t s_test_frame_total_us;
static uint32_t s_test_frame_max_us;
static uint64_t s_test_render_total_us;
static uint64_t s_test_gap_total_us;
static uint64_t s_test_frame_gap_us;
static uint32_t s_test_gap_max_us;
static szpi_display_test_stats_t s_test_stats;

static void display_test_event(lv_event_t *event)
{
    if (!s_test_active) return;
    if (lv_event_get_code(event) != LV_EVENT_RENDER_START &&
        lv_event_get_code(event) != LV_EVENT_RENDER_READY) return;
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(event)) {
        case LV_EVENT_RENDER_START:
            s_test_render_start_us = now;
            break;
        case LV_EVENT_RENDER_READY:
            if (s_test_render_start_us != 0) {
                s_test_render_total_us += now - s_test_render_start_us;
                s_test_renders++;
                s_test_render_start_us = 0;
            }
            break;
        default:
            break;
    }
}

static void record_test_completion(int64_t done_us)
{
    if (!s_test_active || !s_test_flush_tracked) return;
    s_test_last_dma_us = done_us;
    if (!s_test_flush_last) return;
    uint32_t duration = (uint32_t)(done_us - s_test_frame_start_us);
    s_test_frames++;
    s_test_frame_total_us += duration;
    s_test_pixels += s_test_frame_pixels;
    s_test_gap_total_us += s_test_frame_gap_us;
    if (duration > s_test_frame_max_us) s_test_frame_max_us = duration;
    s_test_frame_active = false;
}

static esp_err_t draw_bitmap(const void *rgb565, uint16_t x1, uint16_t y1,
                             uint16_t x2_exclusive, uint16_t y2_exclusive);


static uint32_t display_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool lcd_color_trans_done(esp_lcd_panel_io_handle_t io,
                                 esp_lcd_panel_io_event_data_t *event,
                                 void *context)
{
    (void)io;
    (void)event;
    (void)context;
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (s_flush_semaphore == NULL) return false;
    s_dma_done_us = esp_timer_get_time();
    (void)xSemaphoreGiveFromISR(s_flush_semaphore, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static esp_err_t wait_flush_internal(TickType_t timeout_ticks, bool report_timeout)
{
    if (!s_flush_inflight) return ESP_OK;
    if (xSemaphoreTake(s_flush_semaphore, timeout_ticks) != pdTRUE) {
        if (report_timeout) {
            s_flush_timeouts++;
            ESP_LOGE(TAG, "LCD DMA flush timed out; retaining active buffer");
        }
        return ESP_ERR_TIMEOUT;
    }
    s_flush_inflight = false;
    record_test_completion(s_dma_done_us);
    s_test_flush_tracked = false;
    return ESP_OK;
}

static void flush_wait_cb(lv_display_t *display)
{
    (void)display;
    while (s_flush_inflight) {
        if (wait_flush_internal(pdMS_TO_TICKS(FLUSH_WAIT_SLICE_MS), true) == ESP_OK) break;
    }
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map)
{
    if (area == NULL || pixel_map == NULL || area->x1 < 0 || area->y1 < 0 ||
        area->x2 >= SZPI_DISPLAY_WIDTH || area->y2 >= SZPI_DISPLAY_HEIGHT ||
        area->x1 > area->x2 || area->y1 > area->y2 || s_flush_inflight) {
        s_last_flush_error = ESP_ERR_INVALID_ARG;
        lv_display_flush_ready(display);
        return;
    }
    uint32_t pixels = (uint32_t)(area->x2 - area->x1 + 1) * (uint32_t)(area->y2 - area->y1 + 1);
    // LVGL renders RGB565 in native little-endian memory; ST7789 expects MSB first.
    lv_draw_sw_rgb565_swap(pixel_map, pixels);
    s_test_flush_tracked = s_test_active;
    s_test_flush_last = lv_display_flush_is_last(display);
    if (s_test_active) {
        int64_t now = esp_timer_get_time();
        if (!s_test_frame_active) {
            s_test_frame_active = true;
            s_test_frame_start_us = now;
            s_test_frame_pixels = 0;
            s_test_frame_gap_us = 0;
            s_test_last_dma_us = 0;
        }
        if (s_test_last_dma_us != 0) {
            uint32_t gap = (uint32_t)(now - s_test_last_dma_us);
            s_test_frame_gap_us += gap;
            if (gap > s_test_gap_max_us) s_test_gap_max_us = gap;
        }
        s_test_frame_pixels += pixels;
    }
    s_flush_inflight = true;
    s_last_flush_error = draw_bitmap(pixel_map, (uint16_t)area->x1, (uint16_t)area->y1,
        (uint16_t)(area->x2 + 1), (uint16_t)(area->y2 + 1));
    if (s_last_flush_error != ESP_OK) {
        s_flush_inflight = false;
        s_test_flush_tracked = false;
        s_test_frame_active = false;
        lv_display_flush_ready(display);
        ESP_LOGE(TAG, "LCD flush submission failed: %s", esp_err_to_name(s_last_flush_error));
    }
}

static esp_err_t init_backlight(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = s_bindings->backlight_resolution,
        .timer_num = s_bindings->backlight_timer,
        .freq_hz = s_bindings->backlight_frequency_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;
    s_ledc_timer_initialized = true;
    ledc_channel_config_t channel = {
        .gpio_num = s_bindings->lcd_backlight,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = s_bindings->backlight_channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = s_bindings->backlight_timer,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 1,
    };
    err = ledc_channel_config(&channel);
    if (err == ESP_OK) s_ledc_channel_initialized = true;
    return err;
}

esp_err_t szpi_display_init(lv_display_t **display)
{
    if (display == NULL || s_initialized || s_display != NULL) return ESP_ERR_INVALID_ARG;
    *display = NULL;
    esp_err_t err = szpi_board_get_bindings(&s_bindings);
    if (err != ESP_OK) return err;
    i2c_master_bus_handle_t shared_i2c;
    err = szpi_board_get_i2c_bus(&shared_i2c);
    if (err != ESP_OK) return err;
    (void)shared_i2c;
    if (s_flush_semaphore == NULL) s_flush_semaphore = xSemaphoreCreateCountingStatic(2, 0, &s_flush_semaphore_storage);
    if (s_flush_semaphore == NULL) return ESP_ERR_NO_MEM;
    while (xSemaphoreTake(s_flush_semaphore, 0) == pdTRUE) {}
    s_flush_inflight = false;
    s_flush_timeouts = 0;
    s_last_flush_error = ESP_OK;
    s_test_active = false;
    s_test_frame_active = false;
    s_test_flush_tracked = false;
    s_test_stats = (szpi_display_test_stats_t){0};
    lv_tick_set_cb(display_tick_ms);

    spi_bus_config_t bus = {
        .mosi_io_num = s_bindings->lcd_mosi,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = s_bindings->lcd_sclk,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = SZPI_DISPLAY_BUFFER_BYTES,
    };
    err = spi_bus_initialize(s_bindings->lcd_spi_host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) goto fail;
    s_spi_initialized = true;
    uint32_t spi_clock_hz = CONFIG_SZPI_DISPLAY_SPI_CLOCK_HZ > 0
        ? CONFIG_SZPI_DISPLAY_SPI_CLOCK_HZ : s_bindings->lcd_spi_clock_hz;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = GPIO_NUM_NC,
        .dc_gpio_num = s_bindings->lcd_dc,
        .spi_mode = CONFIG_SZPI_DISPLAY_SPI_MODE,
        .pclk_hz = spi_clock_hz,
        .trans_queue_depth = LCD_QUEUE_DEPTH,
        .on_color_trans_done = lcd_color_trans_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)s_bindings->lcd_spi_host, &io_config, &s_io);
    if (err != ESP_OK) goto fail;

    // The SZPI reference requires reset -> CS low -> panel init. Keep CS
    // released for the reset transaction, which also establishes SPI mode 2
    // before the panel starts receiving commands. There is no independent
    // reset GPIO; this sequence relies on the board's shared hardware reset.
    err = szpi_board_set_lcd_selected(false, pdMS_TO_TICKS(100));
    if (err != ESP_OK) goto fail;

    esp_lcd_panel_dev_config_t panel_config = {
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .reset_gpio_num = GPIO_NUM_NC,
    };
    err = esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) {
        err = szpi_board_set_lcd_selected(true, pdMS_TO_TICKS(100));
        if (err == ESP_OK) s_cs_selected = true;
    }
    if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_invert_color(s_panel, true);
    if (err == ESP_OK) err = esp_lcd_panel_swap_xy(s_panel, true);
    if (err == ESP_OK) err = esp_lcd_panel_mirror(s_panel, true, false);
    if (err == ESP_OK) err = esp_lcd_panel_set_gap(s_panel, 0, 0);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) goto fail;
    s_orientation_inverted = false;
    err = init_backlight();
    if (err != ESP_OK) goto fail;

    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG, "DMA allocation: each=%u free=%u largest=%u",
        (unsigned)SZPI_DISPLAY_BUFFER_BYTES,
        (unsigned)heap_caps_get_free_size(dma_caps),
        (unsigned)heap_caps_get_largest_free_block(dma_caps));
    s_draw_buffer_1 = heap_caps_aligned_alloc(4, SZPI_DISPLAY_BUFFER_BYTES, dma_caps);
    if (s_draw_buffer_1 == NULL) {
        ESP_LOGE(TAG, "DMA buffer 1 allocation failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_draw_buffer_2 = heap_caps_aligned_alloc(4, SZPI_DISPLAY_BUFFER_BYTES, dma_caps);
    if (s_draw_buffer_2 == NULL) {
        ESP_LOGE(TAG, "DMA buffer 2 allocation failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_display = lv_display_create(SZPI_DISPLAY_WIDTH, SZPI_DISPLAY_HEIGHT);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "LVGL display allocation failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, flush_cb);
    lv_display_set_flush_wait_cb(s_display, flush_wait_cb);
    lv_display_add_event_cb(s_display, display_test_event, LV_EVENT_ALL, NULL);
    lv_display_set_buffers(s_display, s_draw_buffer_1, s_draw_buffer_2,
        SZPI_DISPLAY_BUFFER_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    s_initialized = true;
    *display = s_display;
    ESP_LOGI(TAG, "ST7789 initialized %ux%u SPI=%" PRIu32 "Hz mode=%u; two internal DMA buffers=%u bytes; LCD CS held selected",
        SZPI_DISPLAY_WIDTH, SZPI_DISPLAY_HEIGHT, spi_clock_hz, CONFIG_SZPI_DISPLAY_SPI_MODE, (unsigned)(2U * SZPI_DISPLAY_BUFFER_BYTES));
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "display init failed: %s; DMA free=%u largest=%u",
        esp_err_to_name(err),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    (void)szpi_display_deinit();
    return err;
}

esp_err_t szpi_display_set_test_active(bool active)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_test_active = false;
    if (active) {
        esp_err_t err = szpi_display_wait_flush(pdMS_TO_TICKS(1000));
        if (err != ESP_OK) return err;
    }
    s_test_flush_tracked = false;
    s_test_frame_active = false;
    s_test_frame_start_us = 0;
    s_test_last_dma_us = 0;
    s_test_render_start_us = 0;
    s_test_frames = s_test_renders = s_test_pixels = 0;
    s_test_frame_total_us = s_test_render_total_us = s_test_gap_total_us = 0;
    s_test_frame_max_us = s_test_gap_max_us = 0;
    s_test_stats = (szpi_display_test_stats_t){0};
    s_test_window_us = esp_timer_get_time();
    s_test_active = active;
    return ESP_OK;
}

esp_err_t szpi_display_get_test_stats(szpi_display_test_stats_t *stats)
{
    if (stats == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!s_test_active) {
        *stats = s_test_stats;
        return ESP_OK;
    }
    if (s_test_active && s_flush_inflight && wait_flush_internal(0, false) == ESP_OK) {
        lv_display_flush_ready(s_display);
    }
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - s_test_window_us;
    if (s_test_active && elapsed >= 1000000 && s_test_frames != 0) {
        s_test_stats = (szpi_display_test_stats_t){
            .valid = true,
            .fps_x10 = (uint32_t)((uint64_t)s_test_frames * 10000000 / elapsed),
            .frame_avg_us = (uint32_t)(s_test_frame_total_us / s_test_frames),
            .frame_max_us = s_test_frame_max_us,
            .lvgl_avg_us = s_test_renders ? (uint32_t)(s_test_render_total_us / s_test_renders) : 0,
            .gap_avg_us = (uint32_t)(s_test_gap_total_us / s_test_frames),
        };
        ESP_LOGI(TAG, "display test: FPS=%lu.%lu frame avg/max=%lu/%luus LVGL=%luus gap/frame=%luus gap max=%luus pixels/frame=%lu",
            (unsigned long)(s_test_stats.fps_x10 / 10), (unsigned long)(s_test_stats.fps_x10 % 10),
            (unsigned long)s_test_stats.frame_avg_us, (unsigned long)s_test_stats.frame_max_us,
            (unsigned long)s_test_stats.lvgl_avg_us, (unsigned long)s_test_stats.gap_avg_us,
            (unsigned long)s_test_gap_max_us, (unsigned long)(s_test_pixels / s_test_frames));
        s_test_frames = s_test_renders = s_test_pixels = 0;
        s_test_frame_total_us = s_test_render_total_us = s_test_gap_total_us = 0;
        s_test_frame_max_us = s_test_gap_max_us = 0;
        s_test_window_us = now;
    }
    *stats = s_test_stats;
    return ESP_OK;
}

esp_err_t szpi_display_wait_flush(TickType_t timeout_ticks)
{
    if (!s_initialized || s_flush_semaphore == NULL) return ESP_ERR_INVALID_STATE;
    if (!s_flush_inflight) return s_last_flush_error;
    esp_err_t err = wait_flush_internal(timeout_ticks, true);
    if (err == ESP_OK && s_display != NULL) lv_display_flush_ready(s_display);
    return err;
}

esp_err_t szpi_display_get_last_error(void)
{
    return s_last_flush_error;
}

uint32_t szpi_display_get_flush_timeout_count(void)
{
    return s_flush_timeouts;
}

esp_err_t szpi_display_set_orientation_inverted(bool inverted)
{
    if (!s_initialized || s_panel == NULL) return ESP_ERR_INVALID_STATE;
    if (s_orientation_inverted == inverted) return ESP_OK;

    esp_err_t err = szpi_display_wait_flush(pdMS_TO_TICKS(1000));
    if (err != ESP_OK) return err;

    // The normal landscape mapping is mirror_x=true, mirror_y=false. Toggle
    // both MADCTL axes to rotate its physical output by 180 degrees.
    err = esp_lcd_panel_mirror(s_panel, !inverted, inverted);
    if (err == ESP_OK) s_orientation_inverted = inverted;
    return err;
}

static esp_err_t draw_bitmap(const void *rgb565, uint16_t x1, uint16_t y1,
                             uint16_t x2_exclusive, uint16_t y2_exclusive)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (rgb565 == NULL || x1 >= x2_exclusive || y1 >= y2_exclusive ||
        x2_exclusive > SZPI_DISPLAY_WIDTH || y2_exclusive > SZPI_DISPLAY_HEIGHT) return ESP_ERR_INVALID_ARG;
    return esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2_exclusive, y2_exclusive, rgb565);
}

esp_err_t szpi_display_set_brightness(uint8_t percent)
{
    if (!s_initialized || percent > 100) return ESP_ERR_INVALID_ARG;
    uint32_t max_duty = (1U << s_bindings->backlight_resolution) - 1U;
    uint32_t duty = (max_duty * percent) / 100U;
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, s_bindings->backlight_channel, duty);
    if (err == ESP_OK) err = ledc_update_duty(LEDC_LOW_SPEED_MODE, s_bindings->backlight_channel);
    return err;
}

esp_err_t szpi_display_deinit(void)
{
    if (s_flush_inflight) return ESP_ERR_INVALID_STATE;
    esp_err_t result = ESP_OK;
    s_initialized = false;
    s_orientation_inverted = false;
    if (s_display != NULL) {
        lv_display_delete(s_display);
        s_display = NULL;
    }
    if (s_draw_buffer_1 != NULL) { heap_caps_free(s_draw_buffer_1); s_draw_buffer_1 = NULL; }
    if (s_draw_buffer_2 != NULL) { heap_caps_free(s_draw_buffer_2); s_draw_buffer_2 = NULL; }
    if (s_ledc_channel_initialized) {
        esp_err_t err = ledc_stop(LEDC_LOW_SPEED_MODE, s_bindings->backlight_channel, 1);
        if (result == ESP_OK) result = err;
        s_ledc_channel_initialized = false;
    }
    if (s_panel != NULL) {
        esp_err_t err = esp_lcd_panel_del(s_panel);
        if (result == ESP_OK) result = err;
        s_panel = NULL;
    }
    if (s_io != NULL) {
        esp_err_t err = esp_lcd_panel_io_del(s_io);
        if (result == ESP_OK) result = err;
        s_io = NULL;
    }
    if (s_spi_initialized) {
        esp_err_t err = spi_bus_free(s_bindings->lcd_spi_host);
        if (result == ESP_OK) result = err;
        s_spi_initialized = false;
    }
    if (s_cs_selected) {
        esp_err_t err = szpi_board_set_lcd_selected(false, pdMS_TO_TICKS(100));
        if (result == ESP_OK) result = err;
        s_cs_selected = false;
    }
    if (s_ledc_timer_initialized) {
        ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = s_bindings->backlight_timer, .deconfigure = true};
        esp_err_t err = ledc_timer_pause(LEDC_LOW_SPEED_MODE, s_bindings->backlight_timer);
        if (err == ESP_OK) err = ledc_timer_config(&timer);
        if (result == ESP_OK) result = err;
        s_ledc_timer_initialized = false;
    }
    s_bindings = NULL;
    return result;
}
