#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "szpi_board.h"
#include "szpi_input.h"
#include "szpi_input_transform.h"

#define FT6336_ADDRESS 0x38
#define FT6336_TOUCH_COUNT_REG 0x02
#define FT6336_DATA_BYTES 13
#define FT6336_MAX_POINTS 2
#define FT6336_TIMEOUT_MS 20
#define FT6336_FAILURE_LIMIT 5

static i2c_master_dev_handle_t s_device;
static bool s_initialized;
static bool s_primary_valid;
static uint8_t s_primary_id;
static uint16_t s_last_x;
static uint16_t s_last_y;
static uint32_t s_consecutive_errors;

static esp_err_t record_read_failure(szpi_input_state_t *state, esp_err_t error, uint32_t duration_us)
{
    s_consecutive_errors++;
    s_primary_valid = false;
    *state = (szpi_input_state_t){
        .x = s_last_x,
        .y = s_last_y,
        .pressed = false,
        .faulted = s_consecutive_errors >= FT6336_FAILURE_LIMIT,
        .consecutive_errors = s_consecutive_errors,
        .read_duration_us = duration_us,
    };
    return error;
}

static szpi_input_raw_touch_t decode_point(const uint8_t *data)
{
    return (szpi_input_raw_touch_t){
        .x = (uint16_t)(((uint16_t)(data[0] & 0x0FU) << 8) | data[1]),
        .y = (uint16_t)(((uint16_t)(data[2] & 0x0FU) << 8) | data[3]),
        .id = (uint8_t)(data[2] >> 4),
        .event = (uint8_t)(data[0] >> 6),
    };
}

esp_err_t szpi_input_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    i2c_master_bus_handle_t bus;
    esp_err_t err = szpi_board_get_i2c_bus(&bus);
    if (err != ESP_OK) return err;
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FT6336_ADDRESS,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &config, &s_device);
    if (err != ESP_OK) return err;
    s_primary_valid = false;
    s_consecutive_errors = 0;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t szpi_input_read(szpi_input_state_t *state)
{
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    uint8_t reg = FT6336_TOUCH_COUNT_REG;
    uint8_t data[FT6336_DATA_BYTES] = {0};
    int64_t started = esp_timer_get_time();
    esp_err_t err = i2c_master_transmit_receive(s_device, &reg, 1, data, sizeof(data), FT6336_TIMEOUT_MS);
    uint32_t duration = (uint32_t)(esp_timer_get_time() - started);
    if (err != ESP_OK) {
        return record_read_failure(state, err, duration);
    }
    s_consecutive_errors = 0;
    uint8_t count = data[0] & 0x0FU;
    if (count > FT6336_MAX_POINTS) return record_read_failure(state, ESP_ERR_INVALID_RESPONSE, duration);
    if (count == 0) {
        s_primary_valid = false;
        *state = (szpi_input_state_t){.x = s_last_x, .y = s_last_y, .read_duration_us = duration};
        return ESP_OK;
    }

    szpi_input_raw_touch_t points[FT6336_MAX_POINTS] = {
        decode_point(&data[1]),
        decode_point(&data[7]),
    };
    int primary_index = szpi_input_select_primary(points, count, s_primary_valid, s_primary_id);
    if (primary_index < 0) {
        s_primary_valid = false;
        *state = (szpi_input_state_t){.x = s_last_x, .y = s_last_y, .read_duration_us = duration};
        return ESP_OK;
    }
    const szpi_input_raw_touch_t *primary = &points[primary_index];
    s_primary_id = primary->id;
    s_primary_valid = true;
    uint16_t x, y;
    if (!szpi_input_map_coordinates(primary->x, primary->y, &x, &y)) {
        return record_read_failure(state, ESP_ERR_INVALID_RESPONSE, duration);
    }
    s_last_x = x;
    s_last_y = y;
    *state = (szpi_input_state_t){
        .x = x,
        .y = y,
        .touch_count = count,
        .primary_id = s_primary_id,
        .pressed = true,
        .consecutive_errors = 0,
        .read_duration_us = duration,
    };
    return ESP_OK;
}

esp_err_t szpi_input_deinit(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_initialized = false;
    s_primary_valid = false;
    s_consecutive_errors = 0;
    i2c_master_dev_handle_t device = s_device;
    s_device = NULL;
    return i2c_master_bus_rm_device(device);
}
