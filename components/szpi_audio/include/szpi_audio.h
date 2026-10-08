#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define SZPI_AUDIO_SAMPLE_RATE 48000U
#define SZPI_AUDIO_CHANNELS 2U
#define SZPI_AUDIO_BITS_PER_SAMPLE 16U
#define SZPI_AUDIO_BLOCK_BYTES 3840U
#define SZPI_AUDIO_DEFAULT_INPUT_GAIN_DB 36U
#define SZPI_AUDIO_DEFAULT_INPUT_GAIN_PERCENT 100U
#define SZPI_AUDIO_MAX_INPUT_GAIN_DB 36U
#define SZPI_AUDIO_DEFAULT_VOLUME_PERCENT 50U
#define SZPI_AUDIO_MAX_VOLUME_PERCENT 100U

typedef struct {
    bool initialized;
    bool input_open;
    bool output_open;
    uint32_t rx_errors;
    uint32_t tx_errors;
    uint32_t rx_overruns;
    uint32_t tx_underruns;
    uint32_t last_rx_bytes;
    uint32_t last_tx_bytes;
    esp_err_t last_error;
} szpi_audio_status_t;

esp_err_t szpi_audio_init(void);
esp_err_t szpi_audio_deinit(TickType_t timeout_ticks);
esp_err_t szpi_audio_capture_start(float gain_db);
esp_err_t szpi_audio_set_input_gain(float gain_db);
// Volume uses esp_codec_dev's 0-100 percent scale.
esp_err_t szpi_audio_playback_start(int volume_percent);
// Updates the active playback volume; returns INVALID_STATE if playback is closed.
esp_err_t szpi_audio_set_output_volume(int volume_percent);
esp_err_t szpi_audio_capture_stop(TickType_t timeout_ticks);
esp_err_t szpi_audio_playback_stop(TickType_t timeout_ticks);
esp_err_t szpi_audio_read(void *buffer, size_t capacity, size_t *read_bytes, TickType_t timeout_ticks);
esp_err_t szpi_audio_write(const void *buffer, size_t length, size_t *written_bytes, TickType_t timeout_ticks);
esp_err_t szpi_audio_get_status(szpi_audio_status_t *status);
