#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "szpi_app.h"
#include "szpi_audio.h"
#include "szpi_storage.h"
#include "szpi_wav.h"
#include "szpi_runtime_internal.h"

#define TAG "szpi_audio_svc"
#define AUDIO_BLOCK_COUNT 100U
#define CAPTURE_TEST_SECONDS 5U
#define CAPTURE_TEST_BLOCK_COUNT ((SZPI_AUDIO_SAMPLE_RATE * SZPI_AUDIO_CHANNELS * (SZPI_AUDIO_BITS_PER_SAMPLE / 8U) * CAPTURE_TEST_SECONDS) / SZPI_AUDIO_BLOCK_BYTES)
#define CAPTURE_TEST_BUFFER_BYTES (CAPTURE_TEST_BLOCK_COUNT * SZPI_AUDIO_BLOCK_BYTES)
#define TEST_TONE_AMPLITUDE 12000

typedef enum {
    AUDIO_CMD_STOP = 0,
    AUDIO_CMD_TEST_TONE = 1,
    AUDIO_CMD_CAPTURE_TEST = 2,
    AUDIO_CMD_RECORD = 3,
    AUDIO_CMD_PLAY_LATEST = 4,
    AUDIO_CMD_SET_VOLUME = 5,
    AUDIO_CMD_SET_INPUT_GAIN = 6,
    AUDIO_CMD_SAVE_SETTINGS = 7,
} audio_command_t;

static bool s_audio_ready;
static uint8_t s_playback_volume_percent = SZPI_AUDIO_DEFAULT_VOLUME_PERCENT;
static uint8_t s_input_gain_db = SZPI_AUDIO_DEFAULT_INPUT_GAIN_DB;
static uint8_t s_saved_volume_percent = SZPI_AUDIO_DEFAULT_VOLUME_PERCENT;
static uint8_t s_saved_gain_percent = SZPI_AUDIO_DEFAULT_INPUT_GAIN_PERCENT;
static bool s_volume_command_pending;
static bool s_input_gain_command_pending;

static uint8_t gain_percent_to_db(uint8_t percent)
{
    return (uint8_t)(((uint16_t)percent * SZPI_AUDIO_MAX_INPUT_GAIN_DB + 50U) / 100U);
}

static void load_audio_settings(void)
{
    uint8_t volume = SZPI_AUDIO_DEFAULT_VOLUME_PERCENT;
    uint8_t gain_percent = SZPI_AUDIO_DEFAULT_INPUT_GAIN_PERCENT;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("audio", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        esp_err_t volume_err = nvs_get_u8(handle, "speaker_pct", &volume);
        esp_err_t gain_err = nvs_get_u8(handle, "mic_pct", &gain_percent);
        nvs_close(handle);
        if (volume_err != ESP_OK && volume_err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "speaker setting load failed: %s", esp_err_to_name(volume_err));
        }
        if (gain_err != ESP_OK && gain_err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "microphone setting load failed: %s", esp_err_to_name(gain_err));
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "audio settings open failed: %s", esp_err_to_name(err));
    }
    if (volume > SZPI_AUDIO_MAX_VOLUME_PERCENT) volume = SZPI_AUDIO_DEFAULT_VOLUME_PERCENT;
    if (gain_percent > 100U) gain_percent = 100U;
    s_playback_volume_percent = s_saved_volume_percent = volume;
    s_saved_gain_percent = gain_percent;
    s_input_gain_db = gain_percent_to_db(gain_percent);
    if (szpi_audio_status_lock != NULL && xSemaphoreTake(szpi_audio_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        szpi_audio_status.output_volume_percent = volume;
        szpi_audio_status.input_gain_percent = gain_percent;
        szpi_audio_status.input_gain_db = s_input_gain_db;
        xSemaphoreGive(szpi_audio_status_lock);
    }
}

static void save_audio_settings(void)
{
    uint8_t volume;
    uint8_t gain_percent;
    if (szpi_audio_status_lock == NULL || xSemaphoreTake(szpi_audio_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    volume = szpi_audio_status.output_volume_percent;
    gain_percent = szpi_audio_status.input_gain_percent;
    xSemaphoreGive(szpi_audio_status_lock);
    if (volume == s_saved_volume_percent && gain_percent == s_saved_gain_percent) return;

    nvs_handle_t handle;
    esp_err_t err = nvs_open("audio", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "speaker_pct", volume);
        if (err == ESP_OK) err = nvs_set_u8(handle, "mic_pct", gain_percent);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        s_saved_volume_percent = volume;
        s_saved_gain_percent = gain_percent;
    } else {
        ESP_LOGW(TAG, "audio settings save failed: %s", esp_err_to_name(err));
    }
}

static void apply_volume_command(void)
{
    uint8_t volume = s_playback_volume_percent;
    bool output_active = false;
    if (szpi_audio_status_lock != NULL && xSemaphoreTake(szpi_audio_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        volume = szpi_audio_status.output_volume_percent;
        s_volume_command_pending = false;
        output_active = szpi_audio_status.state == SZPI_AUDIO_SERVICE_PLAYING_TEST ||
            szpi_audio_status.state == SZPI_AUDIO_SERVICE_PLAYING_CAPTURE_TEST ||
            szpi_audio_status.state == SZPI_AUDIO_SERVICE_PLAYING_FILE;
        xSemaphoreGive(szpi_audio_status_lock);
    }
    s_playback_volume_percent = volume;
    if (s_audio_ready && output_active) {
        esp_err_t err = szpi_audio_set_output_volume(volume);
        if (err != ESP_OK) ESP_LOGW(TAG, "speaker volume update failed: %s", esp_err_to_name(err));
    }
}

static void apply_input_gain_command(void)
{
    uint8_t gain_db = s_input_gain_db;
    bool input_active = false;
    if (szpi_audio_status_lock != NULL && xSemaphoreTake(szpi_audio_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        gain_db = szpi_audio_status.input_gain_db;
        s_input_gain_command_pending = false;
        input_active = szpi_audio_status.state == SZPI_AUDIO_SERVICE_CAPTURE_TEST ||
            szpi_audio_status.state == SZPI_AUDIO_SERVICE_RECORDING;
        xSemaphoreGive(szpi_audio_status_lock);
    }
    s_input_gain_db = gain_db;
    if (s_audio_ready && input_active) {
        esp_err_t err = szpi_audio_set_input_gain((float)gain_db);
        if (err != ESP_OK) ESP_LOGW(TAG, "microphone gain update failed: %s", esp_err_to_name(err));
    }
}

static uint16_t integer_sqrt(uint64_t value)
{
    uint64_t result = 0;
    uint64_t bit = 1ULL << 62;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result > UINT16_MAX ? UINT16_MAX : (uint16_t)result;
}

static void publish_state(szpi_audio_service_state_t state, esp_err_t error)
{
    if (szpi_audio_status_lock == NULL || xSemaphoreTake(szpi_audio_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    szpi_audio_status.state = state;
    szpi_audio_status.last_error = error;
    if (error != ESP_OK) szpi_audio_status.errors++;
    xSemaphoreGive(szpi_audio_status_lock);
}

static bool take_stop_request(void)
{
    uint32_t command = 0;
    if (xQueueReceive(szpi_audio_queue, &command, 0) != pdTRUE) return false;
    if (command == AUDIO_CMD_STOP) return true;
    if (command == AUDIO_CMD_SET_VOLUME) {
        apply_volume_command();
        return false;
    }
    if (command == AUDIO_CMD_SET_INPUT_GAIN) {
        apply_input_gain_command();
        return false;
    }
    // Keep later actions queued when a test is active; the current test owns the codec until it closes.
    (void)xQueueSend(szpi_audio_queue, &command, 0);
    return false;
}

static void run_tone_test(void)
{
    publish_state(SZPI_AUDIO_SERVICE_PLAYING_TEST, ESP_OK);
    ESP_LOGI(TAG, "440Hz test tone amplitude=%d volume=%u%%", TEST_TONE_AMPLITUDE,
        (unsigned)s_playback_volume_percent);
    esp_err_t err = szpi_audio_playback_start(s_playback_volume_percent);
    static int16_t pcm[SZPI_AUDIO_BLOCK_BYTES / sizeof(int16_t)];
    uint32_t phase = 0;
    for (uint32_t block = 0; err == ESP_OK && block < AUDIO_BLOCK_COUNT; ++block) {
        if (take_stop_request()) break;
        for (size_t i = 0; i < SZPI_AUDIO_BLOCK_BYTES / (2U * sizeof(int16_t)); ++i) {
            int16_t sample;
            if (phase < 12000U) sample = (int16_t)(-TEST_TONE_AMPLITUDE + (int32_t)(phase * (2U * TEST_TONE_AMPLITUDE) / 12000U));
            else if (phase < 36000U) sample = (int16_t)(TEST_TONE_AMPLITUDE - (int32_t)((phase - 12000U) * (2U * TEST_TONE_AMPLITUDE) / 24000U));
            else sample = (int16_t)(-TEST_TONE_AMPLITUDE + (int32_t)((phase - 36000U) * (2U * TEST_TONE_AMPLITUDE) / 12000U));
            pcm[2U * i] = sample;
            pcm[2U * i + 1U] = sample;
            phase += 440U;
            if (phase >= 48000U) phase -= 48000U;
        }
        size_t written = 0;
        err = szpi_audio_write(pcm, sizeof(pcm), &written, pdMS_TO_TICKS(100));
        if (err == ESP_OK && written != sizeof(pcm)) err = ESP_FAIL;
        if (err != ESP_OK) ESP_LOGE(TAG, "test-tone PCM block failed: %s", esp_err_to_name(err));
        if (err == ESP_OK && szpi_audio_status_lock != NULL && xSemaphoreTake(szpi_audio_status_lock, 0) == pdTRUE) {
            szpi_audio_status.blocks_processed++;
            xSemaphoreGive(szpi_audio_status_lock);
        }
    }
    esp_err_t stop_err = szpi_audio_playback_stop(pdMS_TO_TICKS(500));
    if (err == ESP_OK) err = stop_err;
    publish_state(err == ESP_OK ? SZPI_AUDIO_SERVICE_COMPLETE : SZPI_AUDIO_SERVICE_FAULT, err);
}

static void run_capture_test(void)
{
    publish_state(SZPI_AUDIO_SERVICE_CAPTURE_TEST, ESP_OK);
    int16_t *capture = heap_caps_malloc(CAPTURE_TEST_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (capture == NULL) {
        publish_state(SZPI_AUDIO_SERVICE_FAULT, ESP_ERR_NO_MEM);
        return;
    }

    esp_err_t err = szpi_audio_capture_start((float)s_input_gain_db);
    bool capture_started = err == ESP_OK;
    bool cancelled = false;
    static int16_t pcm[SZPI_AUDIO_BLOCK_BYTES / sizeof(int16_t)];
    uint64_t sum_squares = 0;
    uint64_t sample_count = 0;
    uint16_t peak = 0;
    size_t captured_bytes = 0;
    for (uint32_t block = 0; err == ESP_OK && block < CAPTURE_TEST_BLOCK_COUNT; ++block) {
        if (take_stop_request()) {
            cancelled = true;
            break;
        }
        size_t read_bytes = 0;
        err = szpi_audio_read(pcm, sizeof(pcm), &read_bytes, pdMS_TO_TICKS(100));
        if (err != ESP_OK || read_bytes != sizeof(pcm)) { if (err == ESP_OK) err = ESP_FAIL; break; }
        memcpy((uint8_t *)capture + captured_bytes, pcm, read_bytes);
        captured_bytes += read_bytes;
        size_t samples = read_bytes / sizeof(int16_t);
        for (size_t i = 0; i < samples; ++i) {
            int32_t value = pcm[i];
            uint16_t magnitude = (uint16_t)(value < 0 ? -value : value);
            if (magnitude > peak) peak = magnitude;
            sum_squares += (uint64_t)((int64_t)value * value);
        }
        sample_count += samples;
        if (szpi_audio_status_lock != NULL && xSemaphoreTake(szpi_audio_status_lock, 0) == pdTRUE) {
            szpi_audio_status.blocks_processed++;
            szpi_audio_status.peak_sample = peak;
            szpi_audio_status.rms_sample = sample_count == 0 ? 0 : integer_sqrt(sum_squares / sample_count);
            xSemaphoreGive(szpi_audio_status_lock);
        }
    }
    if (capture_started) {
        esp_err_t stop_err = szpi_audio_capture_stop(pdMS_TO_TICKS(500));
        if (err == ESP_OK) err = stop_err;
    }

    if (err == ESP_OK && !cancelled && captured_bytes == CAPTURE_TEST_BUFFER_BYTES) {
        publish_state(SZPI_AUDIO_SERVICE_PLAYING_CAPTURE_TEST, ESP_OK);
        err = szpi_audio_playback_start(s_playback_volume_percent);
        bool playback_started = err == ESP_OK;
        for (size_t offset = 0; err == ESP_OK && offset < captured_bytes; offset += SZPI_AUDIO_BLOCK_BYTES) {
            if (take_stop_request()) {
                cancelled = true;
                break;
            }
            size_t written = 0;
            err = szpi_audio_write((const uint8_t *)capture + offset, SZPI_AUDIO_BLOCK_BYTES,
                &written, pdMS_TO_TICKS(100));
            if (err == ESP_OK && written != SZPI_AUDIO_BLOCK_BYTES) err = ESP_FAIL;
            if (err == ESP_OK && szpi_audio_status_lock != NULL && xSemaphoreTake(szpi_audio_status_lock, 0) == pdTRUE) {
                szpi_audio_status.blocks_processed = (uint32_t)(offset / SZPI_AUDIO_BLOCK_BYTES) + 1U;
                xSemaphoreGive(szpi_audio_status_lock);
            }
        }
        if (playback_started) {
            esp_err_t stop_err = szpi_audio_playback_stop(pdMS_TO_TICKS(500));
            if (err == ESP_OK) err = stop_err;
        }
    } else if (err == ESP_OK && !cancelled) {
        err = ESP_ERR_INVALID_SIZE;
    }
    heap_caps_free(capture);
    publish_state(err == ESP_OK ? SZPI_AUDIO_SERVICE_COMPLETE : SZPI_AUDIO_SERVICE_FAULT, err);
}

static void make_wav_header(uint8_t header[44], uint32_t data_bytes)
{
    (void)szpi_wav_build_pcm_stereo_header(data_bytes, header);
}

static void set_last_file(const char *name)
{
    if (szpi_audio_status_lock == NULL || xSemaphoreTake(szpi_audio_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    (void)strlcpy(szpi_audio_status.last_file, name, sizeof(szpi_audio_status.last_file));
    xSemaphoreGive(szpi_audio_status_lock);
}

static void run_recording(void)
{
    szpi_storage_status_t storage = {0};
    if (szpi_app_storage_get_status(&storage) != ESP_OK || storage.state != SZPI_STORAGE_READY) {
        publish_state(SZPI_AUDIO_SERVICE_FAULT, ESP_ERR_INVALID_STATE);
        return;
    }
    char part_path[48] = {0};
    char final_path[48] = {0};
    uint64_t id = (uint64_t)(esp_timer_get_time() / 1000);
    void *file = NULL;
    for (uint8_t suffix = 0; suffix < 10; ++suffix) {
        if (suffix == 0) (void)snprintf(part_path, sizeof(part_path), "recordings/rec_%" PRIu64 ".part", id);
        else (void)snprintf(part_path, sizeof(part_path), "recordings/rec_%" PRIu64 "_%u.part", id, (unsigned)suffix);
        esp_err_t open_err = szpi_storage_open(part_path, "rb", &file);
        if (open_err == ESP_OK) {
            (void)szpi_storage_close(&file);
            continue;
        }
        (void)snprintf(final_path, sizeof(final_path), "%.*s.wav", (int)(strlen(part_path) - 5U), part_path);
        open_err = szpi_storage_open(part_path, "wb+", &file);
        if (open_err == ESP_OK) break;
    }
    if (file == NULL) { publish_state(SZPI_AUDIO_SERVICE_FAULT, ESP_ERR_NO_MEM); return; }
    set_last_file(part_path);
    uint8_t header[44];
    make_wav_header(header, 0);
    size_t written = 0;
    esp_err_t err = szpi_storage_write(file, header, sizeof(header), &written);
    if (err == ESP_OK && written != sizeof(header)) err = ESP_FAIL;
    if (err == ESP_OK) err = szpi_audio_capture_start((float)s_input_gain_db);
    if (err == ESP_OK) publish_state(SZPI_AUDIO_SERVICE_RECORDING, ESP_OK);
    static int16_t pcm[SZPI_AUDIO_BLOCK_BYTES / sizeof(int16_t)];
    uint64_t data_bytes = 0;
    uint64_t sum_squares = 0;
    uint64_t sample_count = 0;
    uint32_t blocks = 0;
    uint16_t peak = 0;
    while (err == ESP_OK && data_bytes <= UINT32_MAX - 36U - SZPI_AUDIO_BLOCK_BYTES) {
        if (take_stop_request()) break;
        size_t read_bytes = 0;
        err = szpi_audio_read(pcm, sizeof(pcm), &read_bytes, pdMS_TO_TICKS(100));
        if (err != ESP_OK) break;
        if (read_bytes == 0 || read_bytes > sizeof(pcm) || (read_bytes % (SZPI_AUDIO_CHANNELS * sizeof(int16_t))) != 0) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        for (size_t i = 0; i < read_bytes / sizeof(pcm[0]); ++i) {
            int32_t value = pcm[i];
            uint16_t magnitude = (uint16_t)(value < 0 ? -value : value);
            if (magnitude > peak) peak = magnitude;
            sum_squares += (uint64_t)((int64_t)value * value);
        }
        sample_count += read_bytes / sizeof(pcm[0]);
        if (szpi_audio_status_lock != NULL && xSemaphoreTake(szpi_audio_status_lock, 0) == pdTRUE) {
            szpi_audio_status.peak_sample = peak;
            szpi_audio_status.rms_sample = sample_count == 0 ? 0 : integer_sqrt(sum_squares / sample_count);
            xSemaphoreGive(szpi_audio_status_lock);
        }
        written = 0;
        err = szpi_storage_write(file, pcm, read_bytes, &written);
        if (err == ESP_OK && written != read_bytes) err = ESP_FAIL;
        if (err != ESP_OK) break;
        data_bytes += written;
        blocks++;
        if (szpi_audio_status_lock != NULL && xSemaphoreTake(szpi_audio_status_lock, 0) == pdTRUE) {
            szpi_audio_status.blocks_processed = blocks;
            xSemaphoreGive(szpi_audio_status_lock);
        }
    }
    esp_err_t capture_stop_err = szpi_audio_capture_stop(pdMS_TO_TICKS(500));
    if (err == ESP_OK) err = capture_stop_err;
    if (err == ESP_OK) {
        make_wav_header(header, (uint32_t)data_bytes);
        err = szpi_storage_seek(file, 0);
        if (err == ESP_OK) err = szpi_storage_write(file, header, sizeof(header), &written);
        if (err == ESP_OK && written != sizeof(header)) err = ESP_FAIL;
    }
    if (err == ESP_OK) err = szpi_storage_sync(file);
    esp_err_t close_err = szpi_storage_close(&file);
    if (err == ESP_OK) err = close_err;
    if (err == ESP_OK) {
        err = szpi_storage_rename(part_path, final_path);
        if (err == ESP_OK) set_last_file(final_path);
    }
    publish_state(err == ESP_OK ? SZPI_AUDIO_SERVICE_COMPLETE : SZPI_AUDIO_SERVICE_FAULT, err);
}

static bool storage_read_at(void *context, uint64_t offset, void *buffer, size_t length)
{
    void *file = context;
    if (szpi_storage_seek(file, offset) != ESP_OK) return false;
    size_t bytes = 0;
    return szpi_storage_read(file, buffer, length, &bytes) == ESP_OK && bytes == length;
}

static void run_play_latest(void)
{
    char name[sizeof(szpi_audio_status.last_file)] = {0};
    if (szpi_audio_status_lock == NULL || xSemaphoreTake(szpi_audio_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    (void)strlcpy(name, szpi_audio_status.last_file, sizeof(name));
    xSemaphoreGive(szpi_audio_status_lock);
    size_t name_len = strlen(name);
    if (name_len < 4 || strcmp(name + name_len - 4, ".wav") != 0) {
        publish_state(SZPI_AUDIO_SERVICE_FAULT, ESP_ERR_NOT_FOUND);
        return;
    }
    void *file = NULL;
    esp_err_t err = szpi_storage_open(name, "rb", &file);
    uint64_t file_bytes = 0;
    szpi_wav_info_t wav = {0};
    if (err == ESP_OK) err = szpi_storage_size(file, &file_bytes);
    if (err == ESP_OK && !szpi_wav_parse(storage_read_at, file, file_bytes, &wav)) err = ESP_ERR_NOT_SUPPORTED;
    uint16_t channels = wav.channels;
    uint16_t align = (uint16_t)(channels * 2U);
    uint32_t remaining = wav.data_bytes;
    if (err == ESP_OK && szpi_storage_seek(file, wav.data_offset) != ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK) err = szpi_audio_playback_start(s_playback_volume_percent);
    if (err == ESP_OK) publish_state(SZPI_AUDIO_SERVICE_PLAYING_FILE, ESP_OK);
    static int16_t input[SZPI_AUDIO_BLOCK_BYTES / sizeof(int16_t)];
    static int16_t stereo[SZPI_AUDIO_BLOCK_BYTES / sizeof(int16_t)];
    uint32_t blocks = 0;
    while (err == ESP_OK && remaining >= align) {
        if (take_stop_request()) break;
        size_t request = channels == 1 ? sizeof(input) / 2U : sizeof(input);
        if (request > remaining) request = remaining - (remaining % align);
        size_t got = 0;
        err = szpi_storage_read(file, input, request, &got);
        if (err != ESP_OK || got != request || got == 0 || got % align != 0) { if (err == ESP_OK) err = ESP_ERR_INVALID_RESPONSE; break; }
        size_t frames = got / align;
        const int16_t *output = input;
        size_t output_bytes = got;
        if (channels == 1) {
            for (size_t i = 0; i < frames; ++i) { stereo[2U * i] = input[i]; stereo[2U * i + 1U] = input[i]; }
            output = stereo;
            output_bytes = frames * 2U * sizeof(int16_t);
        }
        size_t sent = 0;
        err = szpi_audio_write(output, output_bytes, &sent, pdMS_TO_TICKS(100));
        if (err == ESP_OK && sent != output_bytes) err = ESP_FAIL;
        if (err != ESP_OK) ESP_LOGE(TAG, "WAV PCM block failed: %s", esp_err_to_name(err));
        if (err != ESP_OK) break;
        remaining -= (uint32_t)got;
        blocks++;
        if (szpi_audio_status_lock != NULL && xSemaphoreTake(szpi_audio_status_lock, 0) == pdTRUE) {
            szpi_audio_status.blocks_processed = blocks;
            xSemaphoreGive(szpi_audio_status_lock);
        }
    }
    esp_err_t stop_err = szpi_audio_playback_stop(pdMS_TO_TICKS(500));
    if (err == ESP_OK) err = stop_err;
    if (file != NULL) {
        esp_err_t close_err = szpi_storage_close(&file);
        if (err == ESP_OK) err = close_err;
    }
    publish_state(err == ESP_OK ? SZPI_AUDIO_SERVICE_COMPLETE : SZPI_AUDIO_SERVICE_FAULT, err);
}

void szpi_audio_service_task(void *context)
{
    (void)context;
    (void)xEventGroupWaitBits(szpi_system_events, SZPI_EVENT_RUNTIME_START, pdFALSE, pdTRUE, portMAX_DELAY);
    load_audio_settings();
    for (;;) {
        uint32_t command = 0;
        if (xQueueReceive(szpi_audio_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        if (command == AUDIO_CMD_STOP) continue;
        if (command == AUDIO_CMD_SET_VOLUME) {
            apply_volume_command();
            continue;
        }
        if (command == AUDIO_CMD_SET_INPUT_GAIN) {
            apply_input_gain_command();
            continue;
        }
        if (command == AUDIO_CMD_SAVE_SETTINGS) {
            save_audio_settings();
            continue;
        }
        if (!s_audio_ready) {
            publish_state(SZPI_AUDIO_SERVICE_OFFLINE, ESP_OK);
            esp_err_t err = szpi_audio_init();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "audio initialization failed: %s", esp_err_to_name(err));
                publish_state(SZPI_AUDIO_SERVICE_FAULT, err);
                continue;
            }
            s_audio_ready = true;
            publish_state(SZPI_AUDIO_SERVICE_IDLE, ESP_OK);
        }
        if (szpi_audio_status_lock != NULL && xSemaphoreTake(szpi_audio_status_lock, 0) == pdTRUE) {
            szpi_audio_status.blocks_processed = 0;
            szpi_audio_status.peak_sample = 0;
            szpi_audio_status.rms_sample = 0;
            xSemaphoreGive(szpi_audio_status_lock);
        }
        if (command == AUDIO_CMD_TEST_TONE) run_tone_test();
        else if (command == AUDIO_CMD_CAPTURE_TEST) run_capture_test();
        else if (command == AUDIO_CMD_RECORD) run_recording();
        else if (command == AUDIO_CMD_PLAY_LATEST) run_play_latest();
    }
}

static esp_err_t enqueue_audio(audio_command_t command)
{
    if (szpi_audio_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (command != AUDIO_CMD_STOP) {
        if (szpi_audio_status_lock == NULL) return ESP_ERR_INVALID_STATE;
        if (xSemaphoreTake(szpi_audio_status_lock, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
        bool active = szpi_audio_status.state == SZPI_AUDIO_SERVICE_PLAYING_TEST ||
            szpi_audio_status.state == SZPI_AUDIO_SERVICE_CAPTURE_TEST ||
            szpi_audio_status.state == SZPI_AUDIO_SERVICE_PLAYING_CAPTURE_TEST ||
            szpi_audio_status.state == SZPI_AUDIO_SERVICE_RECORDING ||
            szpi_audio_status.state == SZPI_AUDIO_SERVICE_PLAYING_FILE;
        xSemaphoreGive(szpi_audio_status_lock);
        if (active || uxQueueMessagesWaiting(szpi_audio_queue) != 0) return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(szpi_audio_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t szpi_app_audio_test_tone(void) { return enqueue_audio(AUDIO_CMD_TEST_TONE); }
esp_err_t szpi_app_audio_capture_test(void) { return enqueue_audio(AUDIO_CMD_CAPTURE_TEST); }
esp_err_t szpi_app_audio_record_start(void) { return enqueue_audio(AUDIO_CMD_RECORD); }
esp_err_t szpi_app_audio_play_latest(void) { return enqueue_audio(AUDIO_CMD_PLAY_LATEST); }
esp_err_t szpi_app_audio_stop(void) { return enqueue_audio(AUDIO_CMD_STOP); }

esp_err_t szpi_app_audio_set_volume(uint8_t volume_percent)
{
    if (volume_percent > SZPI_AUDIO_MAX_VOLUME_PERCENT || szpi_audio_queue == NULL ||
            szpi_audio_status_lock == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(szpi_audio_status_lock, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    szpi_audio_status.output_volume_percent = volume_percent;
    if (!s_volume_command_pending) {
        uint32_t command = AUDIO_CMD_SET_VOLUME;
        if (xQueueSend(szpi_audio_queue, &command, 0) != pdTRUE) {
            xSemaphoreGive(szpi_audio_status_lock);
            return ESP_ERR_TIMEOUT;
        }
        s_volume_command_pending = true;
    }
    xSemaphoreGive(szpi_audio_status_lock);
    return ESP_OK;
}

static esp_err_t queue_input_gain(uint8_t gain_db, uint8_t gain_percent)
{
    if (gain_db > SZPI_AUDIO_MAX_INPUT_GAIN_DB || szpi_audio_queue == NULL ||
            szpi_audio_status_lock == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(szpi_audio_status_lock, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    szpi_audio_status.input_gain_db = gain_db;
    szpi_audio_status.input_gain_percent = gain_percent;
    if (!s_input_gain_command_pending) {
        uint32_t command = AUDIO_CMD_SET_INPUT_GAIN;
        if (xQueueSend(szpi_audio_queue, &command, 0) != pdTRUE) {
            xSemaphoreGive(szpi_audio_status_lock);
            return ESP_ERR_TIMEOUT;
        }
        s_input_gain_command_pending = true;
    }
    xSemaphoreGive(szpi_audio_status_lock);
    return ESP_OK;
}

esp_err_t szpi_app_audio_set_input_gain(uint8_t gain_db)
{
    if (gain_db > SZPI_AUDIO_MAX_INPUT_GAIN_DB) return ESP_ERR_INVALID_ARG;
    uint8_t gain_percent = (uint8_t)(((uint16_t)gain_db * 100U +
        SZPI_AUDIO_MAX_INPUT_GAIN_DB / 2U) / SZPI_AUDIO_MAX_INPUT_GAIN_DB);
    return queue_input_gain(gain_db, gain_percent);
}

esp_err_t szpi_app_audio_set_input_gain_percent(uint8_t gain_percent)
{
    if (gain_percent > 100U) return ESP_ERR_INVALID_ARG;
    return queue_input_gain(gain_percent_to_db(gain_percent), gain_percent);
}

esp_err_t szpi_app_audio_save_settings(void)
{
    if (szpi_audio_queue == NULL) return ESP_ERR_INVALID_STATE;
    uint32_t command = AUDIO_CMD_SAVE_SETTINGS;
    return xQueueSend(szpi_audio_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t szpi_app_audio_get_status(szpi_audio_service_status_t *status)
{
    if (status == NULL || szpi_audio_status_lock == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(szpi_audio_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *status = szpi_audio_status;
    xSemaphoreGive(szpi_audio_status_lock);
    return ESP_OK;
}
