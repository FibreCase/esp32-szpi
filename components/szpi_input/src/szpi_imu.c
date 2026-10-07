#include <string.h>
#include <inttypes.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "szpi_board.h"
#include "szpi_input.h"
#include "szpi_imu_math.h"

#define TAG "szpi_imu"
#define QMI_REG_WHO_AM_I 0x00U
#define QMI_REG_REVISION_ID 0x01U
#define QMI_REG_CTRL1 0x02U
#define QMI_REG_CTRL2 0x03U
#define QMI_REG_CTRL3 0x04U
#define QMI_REG_CTRL7 0x08U
#define QMI_REG_CTRL9 0x0AU
#define QMI_REG_CAL1_L 0x0BU
#define QMI_REG_STATUSINT 0x2DU
#define QMI_REG_RST_RESULT 0x4DU
#define QMI_REG_RESET 0x60U
#define QMI_REG_AX_L 0x35U
#define QMI_WHO_AM_I_VALUE 0x05U
#define QMI_RESET_COMMAND 0xB0U
#define QMI_CTRL9_ACK 0x00U
#define QMI_CTRL9_AHB_CLOCK_GATING 0x12U
#define QMI_STATUSINT_LOCKED (1U << 1)
#define QMI_STATUSINT_AVAILABLE (1U << 0)
#define QMI_RESET_OK 0x80U
#define QMI_CTRL1_AUTO_INCREMENT_LITTLE_ENDIAN 0x40U
#define QMI_CTRL2_ACCEL_4G_250HZ 0x15U
#define QMI_CTRL3_GYRO_512DPS_224HZ 0x55U
#define QMI_CTRL7_SYNC_ACCEL_GYRO_ENABLE 0x83U
#define QMI_CTRL7_DISABLE 0x00U
#define QMI_I2C_TIMEOUT_MS 5
#define QMI_ID_PROBE_COUNT 3U
#define QMI_RESET_WAIT_MS 15U
#define QMI_GYRO_START_WAIT_MS 165U
#define QMI_CTRL9_TIMEOUT_MS 50U
#define QMI_LOCK_TIMEOUT_MS 5U
#define QMI_FAILURE_LIMIT 5U
#define QMI_STALE_LIMIT_MS 200U
#define QMI_DISCARD_SAMPLES 3U

static i2c_master_dev_handle_t s_device;
static szpi_imu_sample_t s_last_sample;
static bool s_initialized;
static bool s_ahb_clock_gating_disabled;
static uint8_t s_who_am_i;
static uint8_t s_revision;
static uint32_t s_sample_sequence;
static uint32_t s_consecutive_errors;
static uint8_t s_discard_samples;

static esp_err_t read_registers(uint8_t reg, uint8_t *data, size_t length)
{
    if (s_device == NULL || data == NULL || length == 0) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_device, &reg, 1, data, length, QMI_I2C_TIMEOUT_MS);
}

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    if (s_device == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(s_device, data, sizeof(data), QMI_I2C_TIMEOUT_MS);
}

static esp_err_t read_register(uint8_t reg, uint8_t *value)
{
    return read_registers(reg, value, 1);
}

static esp_err_t write_and_verify(uint8_t reg, uint8_t value, uint8_t verify_mask)
{
    esp_err_t err = write_register(reg, value);
    uint8_t readback = 0;
    if (err == ESP_OK) err = read_register(reg, &readback);
    if (err == ESP_OK && (readback & verify_mask) != (value & verify_mask)) {
        ESP_LOGE(TAG, "register 0x%02x readback 0x%02x expected 0x%02x mask 0x%02x",
            reg, readback, value, verify_mask);
        err = ESP_ERR_INVALID_RESPONSE;
    }
    return err;
}

static esp_err_t run_ctrl9_command(uint8_t command)
{
    esp_err_t err = write_register(QMI_REG_CTRL9, command);
    if (err != ESP_OK) return err;

    int64_t started_us = esp_timer_get_time();
    for (;;) {
        uint8_t status = 0;
        err = read_register(QMI_REG_STATUSINT, &status);
        if (err != ESP_OK) return err;
        if ((status & 0x80U) != 0) break;
        if ((esp_timer_get_time() - started_us) >= (int64_t)QMI_CTRL9_TIMEOUT_MS * 1000) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return write_register(QMI_REG_CTRL9, QMI_CTRL9_ACK);
}

static esp_err_t set_ahb_clock_gating(bool enabled)
{
    esp_err_t err = write_register(QMI_REG_CAL1_L, enabled ? 0x00U : 0x01U);
    if (err == ESP_OK) err = run_ctrl9_command(QMI_CTRL9_AHB_CLOCK_GATING);
    if (err == ESP_OK) s_ahb_clock_gating_disabled = !enabled;
    return err;
}

static esp_err_t restore_clock_gating(void)
{
    if (!s_ahb_clock_gating_disabled) return ESP_OK;
    return set_ahb_clock_gating(true);
}

static void remove_device(void)
{
    if (s_device != NULL) {
        esp_err_t err = i2c_master_bus_rm_device(s_device);
        if (err != ESP_OK) ESP_LOGE(TAG, "failed to remove IMU I2C device: %s", esp_err_to_name(err));
        s_device = NULL;
    }
}

static esp_err_t fail_initialization(esp_err_t cause)
{
    if (s_device != NULL) {
        (void)write_register(QMI_REG_CTRL7, QMI_CTRL7_DISABLE);
        (void)restore_clock_gating();
        remove_device();
    }
    s_initialized = false;
    s_ahb_clock_gating_disabled = false;
    memset(&s_last_sample, 0, sizeof(s_last_sample));
    return cause;
}

esp_err_t szpi_imu_init(szpi_imu_info_t *info)
{
    if (info == NULL) return ESP_ERR_INVALID_ARG;
    *info = (szpi_imu_info_t){0};
    if (s_initialized || s_device != NULL) return ESP_ERR_INVALID_STATE;

    const szpi_board_bindings_t *bindings = NULL;
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = szpi_board_get_bindings(&bindings);
    if (err == ESP_OK) err = szpi_board_get_i2c_bus(&bus);
    if (err != ESP_OK) return err;

    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = bindings->imu_i2c_address,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &config, &s_device);
    if (err != ESP_OK) return err;

    esp_err_t last_probe_error = ESP_ERR_NOT_FOUND;
    bool identity_mismatch = false;
    for (uint8_t attempt = 0; attempt < QMI_ID_PROBE_COUNT; ++attempt) {
        uint8_t who_am_i = 0;
        err = read_register(QMI_REG_WHO_AM_I, &who_am_i);
        if (err == ESP_OK && who_am_i == QMI_WHO_AM_I_VALUE) {
            s_who_am_i = who_am_i;
            last_probe_error = ESP_OK;
            break;
        }
        if (err == ESP_OK) {
            identity_mismatch = true;
            ESP_LOGW(TAG, "WHO_AM_I mismatch attempt=%u value=0x%02x", attempt + 1U, who_am_i);
            last_probe_error = ESP_ERR_NOT_SUPPORTED;
        } else {
            ESP_LOGW(TAG, "WHO_AM_I read failed attempt=%u: %s", attempt + 1U, esp_err_to_name(err));
            last_probe_error = err;
        }
        if (attempt + 1U < QMI_ID_PROBE_COUNT) vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (last_probe_error != ESP_OK) {
        return fail_initialization(identity_mismatch ? ESP_ERR_NOT_SUPPORTED : last_probe_error);
    }

    err = read_register(QMI_REG_REVISION_ID, &s_revision);
    if (err != ESP_OK) return fail_initialization(err);
    err = write_register(QMI_REG_RESET, QMI_RESET_COMMAND);
    if (err != ESP_OK) return fail_initialization(err);
    vTaskDelay(pdMS_TO_TICKS(QMI_RESET_WAIT_MS));
    uint8_t reset_result = 0;
    err = read_register(QMI_REG_RST_RESULT, &reset_result);
    if (err != ESP_OK) return fail_initialization(err);
    if (reset_result != QMI_RESET_OK) {
        ESP_LOGE(TAG, "soft reset verification failed: 0x%02x", reset_result);
        return fail_initialization(ESP_ERR_INVALID_RESPONSE);
    }

    err = write_and_verify(QMI_REG_CTRL1, QMI_CTRL1_AUTO_INCREMENT_LITTLE_ENDIAN, 0x60U);
    if (err == ESP_OK) err = write_and_verify(QMI_REG_CTRL2, QMI_CTRL2_ACCEL_4G_250HZ, 0x7FU);
    if (err == ESP_OK) err = write_and_verify(QMI_REG_CTRL3, QMI_CTRL3_GYRO_512DPS_224HZ, 0x7FU);
    if (err == ESP_OK) err = set_ahb_clock_gating(false);
    if (err == ESP_OK) err = write_and_verify(QMI_REG_CTRL7, QMI_CTRL7_SYNC_ACCEL_GYRO_ENABLE, 0x83U);
    if (err != ESP_OK) return fail_initialization(err);

    // QST specifies 150 ms gyro wake-up plus 3/ODR filter settling.
    vTaskDelay(pdMS_TO_TICKS(QMI_GYRO_START_WAIT_MS));
    s_sample_sequence = 0;
    s_consecutive_errors = 0;
    s_discard_samples = QMI_DISCARD_SAMPLES;
    memset(&s_last_sample, 0, sizeof(s_last_sample));
    s_initialized = true;
    info->who_am_i = s_who_am_i;
    info->revision = s_revision;
    ESP_LOGI(TAG, "QMI8658A ready WHO_AM_I=0x%02x revision=0x%02x acc=+/-4g@250Hz gyro=+/-512dps@224.2Hz",
        s_who_am_i, s_revision);
    return ESP_OK;
}

static void publish_read_error(szpi_imu_sample_t *sample, esp_err_t error, uint32_t duration_us)
{
    s_consecutive_errors++;
    int64_t now_us = esp_timer_get_time();
    s_last_sample.fresh = false;
    s_last_sample.valid = s_last_sample.sequence != 0 &&
        (uint64_t)(now_us - s_last_sample.timestamp_us) <= (uint64_t)QMI_STALE_LIMIT_MS * 1000ULL;
    s_last_sample.read_duration_us = duration_us;
    s_last_sample.consecutive_errors = s_consecutive_errors;
    s_last_sample.last_error = error;
    s_last_sample.faulted = s_consecutive_errors >= QMI_FAILURE_LIMIT;
    *sample = s_last_sample;
}

static esp_err_t read_locked_sample(uint8_t data[12])
{
    int64_t started_us = esp_timer_get_time();
    for (;;) {
        uint8_t status = 0;
        esp_err_t err = read_register(QMI_REG_STATUSINT, &status);
        if (err != ESP_OK) return err;
        if ((status & QMI_STATUSINT_AVAILABLE) == 0) return ESP_ERR_NOT_FOUND;
        if ((status & QMI_STATUSINT_LOCKED) != 0) break;
        if ((esp_timer_get_time() - started_us) >= (int64_t)QMI_LOCK_TIMEOUT_MS * 1000) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return read_registers(QMI_REG_AX_L, data, 12);
}

esp_err_t szpi_imu_read(szpi_imu_sample_t *sample)
{
    if (sample == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || s_device == NULL) return ESP_ERR_INVALID_STATE;
    if (s_last_sample.faulted) {
        *sample = s_last_sample;
        return ESP_ERR_INVALID_STATE;
    }

    int64_t started_us = esp_timer_get_time();
    uint8_t data[12] = {0};
    esp_err_t err = read_locked_sample(data);
    uint32_t duration_us = (uint32_t)(esp_timer_get_time() - started_us);
    if (err == ESP_ERR_NOT_FOUND) {
        s_consecutive_errors = 0;
        s_last_sample.fresh = false;
        s_last_sample.valid = s_last_sample.sequence != 0 &&
            (uint64_t)(esp_timer_get_time() - s_last_sample.timestamp_us) <=
                (uint64_t)QMI_STALE_LIMIT_MS * 1000ULL;
        s_last_sample.read_duration_us = duration_us;
        s_last_sample.consecutive_errors = 0;
        s_last_sample.last_error = ESP_OK;
        *sample = s_last_sample;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        publish_read_error(sample, err, duration_us);
        ESP_LOGW(TAG, "sample read failed count=%" PRIu32 "/%u: %s",
            s_consecutive_errors, QMI_FAILURE_LIMIT, esp_err_to_name(err));
        return err;
    }

    s_consecutive_errors = 0;
    if (s_discard_samples > 0) {
        s_discard_samples--;
        s_last_sample.fresh = false;
        s_last_sample.valid = false;
        s_last_sample.read_duration_us = duration_us;
        s_last_sample.consecutive_errors = 0;
        s_last_sample.last_error = ESP_OK;
        *sample = s_last_sample;
        return ESP_OK;
    }

    szpi_imu_sample_t next = {0};
    for (size_t axis = 0; axis < 3; ++axis) {
        next.accel_raw[axis] = szpi_imu_decode_i16_le(data[axis * 2U], data[axis * 2U + 1U]);
        next.gyro_raw[axis] = szpi_imu_decode_i16_le(data[6U + axis * 2U], data[7U + axis * 2U]);
        next.accel_g[axis] = szpi_imu_accel_to_g(next.accel_raw[axis]);
        next.gyro_dps[axis] = szpi_imu_gyro_to_dps(next.gyro_raw[axis]);
    }
    next.tilt_reliable = szpi_imu_compute_tilt(next.accel_g, &next.roll_deg, &next.pitch_deg);
    next.sequence = ++s_sample_sequence;
    if (next.sequence == 0) next.sequence = ++s_sample_sequence;
    next.read_duration_us = duration_us;
    next.consecutive_errors = 0;
    next.timestamp_us = esp_timer_get_time();
    next.last_error = ESP_OK;
    next.valid = true;
    next.fresh = true;
    s_last_sample = next;
    *sample = next;
    return ESP_OK;
}

esp_err_t szpi_imu_deinit(void)
{
    if (!s_initialized || s_device == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t result = write_register(QMI_REG_CTRL7, QMI_CTRL7_DISABLE);
    if (result != ESP_OK) ESP_LOGW(TAG, "failed to disable sensors during stop: %s", esp_err_to_name(result));
    esp_err_t err = restore_clock_gating();
    if (result == ESP_OK) result = err;
    remove_device();
    s_initialized = false;
    s_ahb_clock_gating_disabled = false;
    s_consecutive_errors = 0;
    s_discard_samples = 0;
    memset(&s_last_sample, 0, sizeof(s_last_sample));
    return result;
}
