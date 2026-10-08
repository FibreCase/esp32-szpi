#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2s_tdm.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "szpi_board.h"
#include "szpi_audio.h"

#define TAG "szpi_audio"
#define AUDIO_I2C_SPEED_HZ 100000

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static const audio_codec_ctrl_if_t *s_tx_ctrl;
static const audio_codec_ctrl_if_t *s_rx_ctrl;
static const audio_codec_data_if_t *s_data;
static const audio_codec_if_t *s_es8311;
static const audio_codec_if_t *s_es7210;
static const audio_codec_gpio_if_t *s_gpio_if;
static esp_codec_dev_handle_t s_tx_dev;
static esp_codec_dev_handle_t s_rx_dev;
static bool s_initialized;
static bool s_rx_open;
static bool s_tx_open;
static bool s_pa_enabled;
static szpi_audio_status_t s_status;

static void release_codec_objects(void)
{
    if (s_tx_dev != NULL) { esp_codec_dev_delete(s_tx_dev); s_tx_dev = NULL; }
    if (s_rx_dev != NULL) { esp_codec_dev_delete(s_rx_dev); s_rx_dev = NULL; }
    if (s_es8311 != NULL) { (void)audio_codec_delete_codec_if(s_es8311); s_es8311 = NULL; }
    if (s_es7210 != NULL) { (void)audio_codec_delete_codec_if(s_es7210); s_es7210 = NULL; }
    if (s_data != NULL) { (void)audio_codec_delete_data_if(s_data); s_data = NULL; }
    if (s_tx_ctrl != NULL) { (void)audio_codec_delete_ctrl_if(s_tx_ctrl); s_tx_ctrl = NULL; }
    if (s_rx_ctrl != NULL) { (void)audio_codec_delete_ctrl_if(s_rx_ctrl); s_rx_ctrl = NULL; }
    if (s_gpio_if != NULL) { (void)audio_codec_delete_gpio_if(s_gpio_if); s_gpio_if = NULL; }
}

esp_err_t szpi_audio_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    }
    const szpi_board_bindings_t *bindings = NULL;
    ESP_RETURN_ON_ERROR(szpi_board_get_bindings(&bindings), TAG, "get board bindings");
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(szpi_board_get_i2c_bus(&bus), TAG, "borrow board I2C");
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    esp_err_t err = i2s_new_channel(&channel_config, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S0 channel allocation failed: %s (internal free=%u, largest=%u)",
            esp_err_to_name(err), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        goto fail;
    }
    i2s_std_config_t tx_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SZPI_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = bindings->audio_mclk, .bclk = bindings->audio_bclk, .ws = bindings->audio_ws,
            .dout = bindings->audio_dout, .din = I2S_GPIO_UNUSED},
    };
    tx_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_std_mode(s_tx, &tx_config);
    if (err != ESP_OK) { ESP_LOGE(TAG, "I2S0 TX STD init failed: %s", esp_err_to_name(err)); goto fail; }
    i2s_tdm_config_t rx_config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(SZPI_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1),
        .gpio_cfg = {.mclk = bindings->audio_mclk, .bclk = bindings->audio_bclk, .ws = bindings->audio_ws,
            .dout = I2S_GPIO_UNUSED, .din = bindings->audio_din},
    };
    rx_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_tdm_mode(s_rx, &rx_config);
    if (err != ESP_OK) { ESP_LOGE(TAG, "I2S0 RX TDM init failed: %s", esp_err_to_name(err)); goto fail; }

    // esp_codec_dev's I2C adapter takes the codec's 8-bit register address and
    // shifts it right once for IDF's 7-bit I2C API. Board bindings stay 7-bit.
    audio_codec_i2c_cfg_t i2c_tx = {.port = bindings->i2c_port,
        .addr = (uint8_t)(bindings->es8311_i2c_address << 1), .bus_handle = bus, .clock_speed_hz = AUDIO_I2C_SPEED_HZ};
    audio_codec_i2c_cfg_t i2c_rx = {.port = bindings->i2c_port,
        .addr = (uint8_t)(bindings->es7210_i2c_address << 1), .bus_handle = bus, .clock_speed_hz = AUDIO_I2C_SPEED_HZ};
    s_tx_ctrl = audio_codec_new_i2c_ctrl(&i2c_tx);
    s_rx_ctrl = audio_codec_new_i2c_ctrl(&i2c_rx);
    if (s_tx_ctrl == NULL || s_rx_ctrl == NULL) {
        ESP_LOGE(TAG, "codec I2C interface allocation failed (ES8311=0x%02x, ES7210=0x%02x)",
            bindings->es8311_i2c_address, bindings->es7210_i2c_address);
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    audio_codec_i2s_cfg_t data_cfg = {.port = I2S_NUM_0, .tx_handle = s_tx, .rx_handle = s_rx};
    s_data = audio_codec_new_i2s_data(&data_cfg);
    if (s_data == NULL) {
        ESP_LOGE(TAG, "I2S codec data interface allocation failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == NULL) { ESP_LOGE(TAG, "codec GPIO interface allocation failed"); err = ESP_ERR_NO_MEM; goto fail; }
    es8311_codec_cfg_t es8311_cfg = {.ctrl_if = s_tx_ctrl, .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC, .master_mode = false, .use_mclk = true,
        .pa_pin = -1, .pa_reverted = false, .mclk_div = I2S_MCLK_MULTIPLE_256};
    s_es8311 = es8311_codec_new(&es8311_cfg);
    es7210_codec_cfg_t es7210_cfg = {.ctrl_if = s_rx_ctrl, .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2, .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = I2S_MCLK_MULTIPLE_256};
    s_es7210 = es7210_codec_new(&es7210_cfg);
    if (s_es8311 == NULL || s_es7210 == NULL) { ESP_LOGE(TAG, "codec driver allocation failed"); err = ESP_ERR_NO_MEM; goto fail; }
    esp_codec_dev_cfg_t tx_dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = s_es8311, .data_if = s_data};
    esp_codec_dev_cfg_t rx_dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = s_es7210, .data_if = s_data};
    s_tx_dev = esp_codec_dev_new(&tx_dev_cfg);
    s_rx_dev = esp_codec_dev_new(&rx_dev_cfg);
    if (s_tx_dev == NULL || s_rx_dev == NULL) { ESP_LOGE(TAG, "codec device allocation failed"); err = ESP_ERR_NO_MEM; goto fail; }
    s_initialized = true;
    s_status = (szpi_audio_status_t){.initialized = true};
    ESP_LOGI(TAG, "audio ready: I2S0 shared clock 48kHz, 16-bit, ES7210 MIC1/2 and ES8311 DAC");
    return ESP_OK;
fail:
    release_codec_objects();
    if (s_rx != NULL) { (void)i2s_del_channel(s_rx); s_rx = NULL; }
    if (s_tx != NULL) { (void)i2s_del_channel(s_tx); s_tx = NULL; }
    return err;
}

esp_err_t szpi_audio_deinit(TickType_t timeout_ticks)
{
    if (!s_initialized || s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, timeout_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_rx_open || s_tx_open) { xSemaphoreGive(s_mutex); return ESP_ERR_INVALID_STATE; }
    release_codec_objects();
    esp_err_t err = s_rx != NULL ? i2s_del_channel(s_rx) : ESP_OK;
    s_rx = NULL;
    if (s_tx != NULL) {
        esp_err_t tx_err = i2s_del_channel(s_tx);
        if (err == ESP_OK) err = tx_err;
    }
    s_tx = NULL;
    s_initialized = false;
    s_status.initialized = false;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t szpi_audio_capture_start(float gain_db)
{
    if (!s_initialized || gain_db < 0.0f || gain_db > 36.0f) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_rx_open) { xSemaphoreGive(s_mutex); return ESP_ERR_INVALID_STATE; }
    esp_codec_dev_sample_info_t sample = {.bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT, .channel = 2,
        .channel_mask = ES7210_SEL_MIC1 | ES7210_SEL_MIC2, .sample_rate = SZPI_AUDIO_SAMPLE_RATE};
    int codec_err = esp_codec_dev_open(s_rx_dev, &sample);
    bool opened = codec_err == ESP_CODEC_DEV_OK;
    if (codec_err == ESP_CODEC_DEV_OK) codec_err = esp_codec_dev_set_in_gain(s_rx_dev, gain_db);
    esp_err_t err = codec_err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) { s_rx_open = true; s_status.input_open = true; }
    else if (opened) (void)esp_codec_dev_close(s_rx_dev);
    s_status.last_error = err;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t szpi_audio_set_input_gain(float gain_db)
{
    if (!s_initialized || gain_db < 0.0f || gain_db > SZPI_AUDIO_MAX_INPUT_GAIN_DB) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_rx_open) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    int codec_err = esp_codec_dev_set_in_gain(s_rx_dev, gain_db);
    esp_err_t err = codec_err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    s_status.last_error = err;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t szpi_audio_playback_start(int volume_percent)
{
    if (!s_initialized || volume_percent < 0 || volume_percent > SZPI_AUDIO_MAX_VOLUME_PERCENT) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_tx_open) { xSemaphoreGive(s_mutex); return ESP_ERR_INVALID_STATE; }
    esp_codec_dev_sample_info_t sample = {.bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT, .channel = 2,
        .channel_mask = 0x03, .sample_rate = SZPI_AUDIO_SAMPLE_RATE};
    int codec_err = esp_codec_dev_open(s_tx_dev, &sample);
    bool opened = codec_err == ESP_CODEC_DEV_OK;
    if (codec_err == ESP_CODEC_DEV_OK) codec_err = esp_codec_dev_set_out_vol(s_tx_dev, volume_percent);
    esp_err_t err = codec_err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) { s_tx_open = true; s_status.output_open = true; }
    else if (opened) (void)esp_codec_dev_close(s_tx_dev);
    if (err == ESP_OK) ESP_LOGI(TAG, "playback codec opened at volume=%d%%", volume_percent);
    s_status.last_error = err;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t szpi_audio_set_output_volume(int volume_percent)
{
    if (!s_initialized || volume_percent < 0 || volume_percent > SZPI_AUDIO_MAX_VOLUME_PERCENT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_tx_open) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    int codec_err = esp_codec_dev_set_out_vol(s_tx_dev, volume_percent);
    esp_err_t err = codec_err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    s_status.last_error = err;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t szpi_audio_capture_stop(TickType_t timeout_ticks)
{
    if (!s_initialized || xSemaphoreTake(s_mutex, timeout_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_rx_open) { xSemaphoreGive(s_mutex); return ESP_ERR_INVALID_STATE; }
    int result = esp_codec_dev_close(s_rx_dev);
    if (result == ESP_CODEC_DEV_OK) { s_rx_open = false; s_status.input_open = false; }
    xSemaphoreGive(s_mutex);
    return result == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t szpi_audio_playback_stop(TickType_t timeout_ticks)
{
    if (!s_initialized || xSemaphoreTake(s_mutex, timeout_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t pa_err = szpi_board_set_amplifier_enabled(false, timeout_ticks);
    s_pa_enabled = false;
    if (!s_tx_open) { xSemaphoreGive(s_mutex); return pa_err; }
    int codec_err = esp_codec_dev_close(s_tx_dev);
    if (codec_err == ESP_CODEC_DEV_OK) { s_tx_open = false; s_status.output_open = false; }
    xSemaphoreGive(s_mutex);
    return pa_err != ESP_OK ? pa_err : (codec_err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
}

esp_err_t szpi_audio_read(void *buffer, size_t capacity, size_t *read_bytes, TickType_t timeout_ticks)
{
    if (buffer == NULL || read_bytes == NULL || capacity == 0 || !s_rx_open) return ESP_ERR_INVALID_ARG;
    if (capacity % (SZPI_AUDIO_CHANNELS * sizeof(int16_t)) != 0) return ESP_ERR_INVALID_SIZE;
    if (xSemaphoreTake(s_mutex, timeout_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    int err = esp_codec_dev_read(s_rx_dev, buffer, (int)capacity);
    *read_bytes = err == ESP_CODEC_DEV_OK ? capacity : 0;
    if (err != ESP_CODEC_DEV_OK) { s_status.rx_errors++; s_status.last_error = ESP_FAIL; }
    else s_status.last_rx_bytes = (uint32_t)*read_bytes;
    xSemaphoreGive(s_mutex);
    return err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t szpi_audio_write(const void *buffer, size_t length, size_t *written_bytes, TickType_t timeout_ticks)
{
    if (buffer == NULL || written_bytes == NULL || length == 0 || !s_tx_open) return ESP_ERR_INVALID_ARG;
    if (length % (SZPI_AUDIO_CHANNELS * sizeof(int16_t)) != 0) return ESP_ERR_INVALID_SIZE;
    if (xSemaphoreTake(s_mutex, timeout_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_pa_enabled) {
        esp_err_t pa_err = szpi_board_set_amplifier_enabled(true, pdMS_TO_TICKS(100));
        if (pa_err != ESP_OK) {
            ESP_LOGE(TAG, "PA_EN could not be asserted before PCM output: %s", esp_err_to_name(pa_err));
            xSemaphoreGive(s_mutex);
            return pa_err;
        }
        s_pa_enabled = true;
        ESP_LOGI(TAG, "PA enabled for PCM output");
    }
    int err = esp_codec_dev_write(s_tx_dev, (void *)buffer, (int)length);
    *written_bytes = err == ESP_CODEC_DEV_OK ? length : 0;
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec PCM write failed: %d", err);
        s_status.tx_errors++;
        s_status.last_error = ESP_FAIL;
        (void)szpi_board_set_amplifier_enabled(false, pdMS_TO_TICKS(100));
        s_pa_enabled = false;
    }
    else s_status.last_tx_bytes = (uint32_t)*written_bytes;
    xSemaphoreGive(s_mutex);
    return err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t szpi_audio_get_status(szpi_audio_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *status = s_status;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
