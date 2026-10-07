#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    gpio_num_t i2c_sda;
    gpio_num_t i2c_scl;
    gpio_num_t lcd_backlight;
    gpio_num_t lcd_mosi;
    gpio_num_t lcd_sclk;
    gpio_num_t lcd_dc;
    uint8_t i2c_port;
    uint8_t pca9557_address;
    uint8_t lcd_cs_bit;
    uint8_t amplifier_enable_bit;
    uint8_t camera_pwdn_bit;
    spi_host_device_t lcd_spi_host;
    uint32_t lcd_spi_clock_hz;
    ledc_timer_t backlight_timer;
    ledc_channel_t backlight_channel;
    uint32_t backlight_frequency_hz;
    ledc_timer_bit_t backlight_resolution;
} szpi_board_bindings_t;

// Bindings are immutable. Keep board initialized while any peripheral uses its bus.
// Callers must stop all board clients before deinit.
esp_err_t szpi_board_get_bindings(const szpi_board_bindings_t **bindings);
esp_err_t szpi_board_get_i2c_bus(i2c_master_bus_handle_t *bus);
esp_err_t szpi_board_init(void);
esp_err_t szpi_board_deinit(void);
esp_err_t szpi_board_set_lcd_selected(bool selected, TickType_t timeout_ticks);
esp_err_t szpi_board_set_amplifier_enabled(bool enabled, TickType_t timeout_ticks);
esp_err_t szpi_board_set_camera_powered(bool powered, TickType_t timeout_ticks);
