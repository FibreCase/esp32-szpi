#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "szpi_board.h"

#define TAG "szpi_board"
#define PCA_REG_INPUT 0x00
#define PCA_REG_OUTPUT 0x01
#define PCA_REG_POLARITY 0x02
#define PCA_REG_CONFIG 0x03
#define PCA_SAFE_OUTPUT 0x05
#define PCA_POLARITY 0x00
#define PCA_CONFIG 0xF8
#define I2C_TIMEOUT_MS 100

static const szpi_board_bindings_t s_bindings = {
    .i2c_sda = GPIO_NUM_1,
    .i2c_scl = GPIO_NUM_2,
    .lcd_backlight = GPIO_NUM_42,
    .lcd_mosi = GPIO_NUM_40,
    .lcd_sclk = GPIO_NUM_41,
    .lcd_dc = GPIO_NUM_39,
    .camera_data = {GPIO_NUM_16, GPIO_NUM_18, GPIO_NUM_8, GPIO_NUM_17,
                    GPIO_NUM_15, GPIO_NUM_6, GPIO_NUM_4, GPIO_NUM_9},
    .camera_vsync = GPIO_NUM_3,
    .camera_href = GPIO_NUM_46,
    .camera_pclk = GPIO_NUM_7,
    .camera_xclk = GPIO_NUM_5,
    .boot_button_gpio = GPIO_NUM_0,
    .boot_button_active_low = true,
    .i2c_port = I2C_NUM_0,
    .pca9557_address = 0x19,
    .lcd_cs_bit = 0,
    .amplifier_enable_bit = 1,
    .camera_pwdn_bit = 2,
    .lcd_spi_host = SPI2_HOST,
    .lcd_spi_clock_hz = 20 * 1000 * 1000,
    .backlight_timer = LEDC_TIMER_1,
    .backlight_channel = LEDC_CHANNEL_1,
    .backlight_frequency_hz = 5000,
    .backlight_resolution = LEDC_TIMER_10_BIT,
    .camera_xclk_timer = LEDC_TIMER_0,
    .camera_xclk_channel = LEDC_CHANNEL_0,
    .camera_xclk_frequency_hz = 20 * 1000 * 1000,
    .camera_sccb_address = 0x3C,
    .camera_pid = 0x2145,
    .imu_i2c_address = 0x6A,
};
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pca;
static uint8_t s_output = PCA_SAFE_OUTPUT;
static bool s_initialized;

static esp_err_t pca_write(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_pca, data, sizeof(data), I2C_TIMEOUT_MS);
}

static esp_err_t pca_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_pca, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

static esp_err_t set_output_bit(uint8_t bit, bool high, TickType_t timeout_ticks)
{
    if (!s_initialized || xSemaphoreTake(s_lock, timeout_ticks) != pdTRUE) {
        return s_initialized ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }
    uint8_t next = high ? (uint8_t)(s_output | (1U << bit)) : (uint8_t)(s_output & ~(1U << bit));
    esp_err_t err = pca_write(PCA_REG_OUTPUT, next);
    if (err == ESP_OK) {
        s_output = next;
    }
    uint8_t input = 0, output = 0, config = 0;
    esp_err_t readback_error = ESP_OK;
    if (err == ESP_OK && bit == s_bindings.lcd_cs_bit) {
        readback_error = pca_read(PCA_REG_INPUT, &input);
        if (readback_error == ESP_OK) readback_error = pca_read(PCA_REG_OUTPUT, &output);
        if (readback_error == ESP_OK) readback_error = pca_read(PCA_REG_CONFIG, &config);
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK && bit == s_bindings.lcd_cs_bit) {
        if (readback_error == ESP_OK) {
            ESP_LOGI(TAG, "LCD CS requested=%u pin=%u output=0x%02x config=0x%02x",
                (unsigned)high, (unsigned)((input >> bit) & 1U), output, config);
        } else {
            ESP_LOGW(TAG, "LCD CS write succeeded; readback failed: %s", esp_err_to_name(readback_error));
        }
    }
    return err;
}

esp_err_t szpi_board_get_bindings(const szpi_board_bindings_t **bindings)
{
    if (bindings == NULL) return ESP_ERR_INVALID_ARG;
    *bindings = &s_bindings;
    return ESP_OK;
}

esp_err_t szpi_board_get_i2c_bus(i2c_master_bus_handle_t *bus)
{
    if (bus == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || s_bus == NULL) return ESP_ERR_INVALID_STATE;
    *bus = s_bus;
    return ESP_OK;
}

esp_err_t szpi_board_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = s_bindings.i2c_sda,
        .scl_io_num = s_bindings.i2c_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) goto fail;

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_bindings.pca9557_address,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &device_config, &s_pca);
    if (err != ESP_OK) goto fail;

    gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << s_bindings.lcd_backlight,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&backlight_config);
    if (err != ESP_OK) goto fail;
    err = gpio_set_level(s_bindings.lcd_backlight, 1);
    if (err != ESP_OK) goto fail;

    // Set latch before direction to avoid glitches on active outputs.
    err = pca_write(PCA_REG_OUTPUT, PCA_SAFE_OUTPUT);
    if (err != ESP_OK) goto fail;
    s_output = PCA_SAFE_OUTPUT;
    err = pca_write(PCA_REG_POLARITY, PCA_POLARITY);
    if (err != ESP_OK) goto fail;
    err = pca_write(PCA_REG_CONFIG, PCA_CONFIG);
    if (err != ESP_OK) goto fail;

    uint8_t output = 0, polarity = 0, config = 0;
    err = pca_read(PCA_REG_OUTPUT, &output);
    if (err == ESP_OK) err = pca_read(PCA_REG_POLARITY, &polarity);
    if (err == ESP_OK) err = pca_read(PCA_REG_CONFIG, &config);
    if (err != ESP_OK) goto fail;
    if (output != PCA_SAFE_OUTPUT || polarity != PCA_POLARITY || config != PCA_CONFIG) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "PCA9557 readback output=0x%02x polarity=0x%02x config=0x%02x", output, polarity, config);
    return ESP_OK;

fail:
    if (s_pca != NULL) { i2c_master_bus_rm_device(s_pca); s_pca = NULL; }
    if (s_bus != NULL) { i2c_del_master_bus(s_bus); s_bus = NULL; }
    if (s_lock != NULL) { vSemaphoreDelete(s_lock); s_lock = NULL; }
    return err;
}

esp_err_t szpi_board_deinit(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_initialized = false;
    esp_err_t err = i2c_master_bus_rm_device(s_pca);
    esp_err_t bus_err = i2c_del_master_bus(s_bus);
    s_pca = NULL;
    s_bus = NULL;
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    return err != ESP_OK ? err : bus_err;
}

esp_err_t szpi_board_set_lcd_selected(bool selected, TickType_t timeout_ticks)
{
    return set_output_bit(s_bindings.lcd_cs_bit, !selected, timeout_ticks);
}

esp_err_t szpi_board_set_amplifier_enabled(bool enabled, TickType_t timeout_ticks)
{
    return set_output_bit(s_bindings.amplifier_enable_bit, enabled, timeout_ticks);
}

esp_err_t szpi_board_set_camera_powered(bool powered, TickType_t timeout_ticks)
{
    return set_output_bit(s_bindings.camera_pwdn_bit, !powered, timeout_ticks);
}
