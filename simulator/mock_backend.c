#include "mock_backend.h"

#include <time.h>

static szpi_ui_model_t s_model;
static uint32_t s_boot_short_presses;

void mock_backend_init(void)
{
    s_model = (szpi_ui_model_t){
        .display_brightness_percent = 50,
        .speaker_volume_percent = 50,
        .microphone_gain_percent = 100,
        .audio_state = SZPI_UI_AUDIO_IDLE,
        .storage_state = SZPI_UI_STORAGE_READY,
        .storage_capacity_bytes = 32ULL * 1024ULL * 1024ULL * 1024ULL,
        .storage_free_bytes = 24ULL * 1024ULL * 1024ULL * 1024ULL,
        .storage_generation = 1,
        .storage_max_frequency_khz = 20000,
        .storage_fat_type = 3,
        .imu_sequence = 1,
        .accel_g = {0.0f, 0.0f, 1.0f},
        .imu_available = true,
        .imu_valid = true,
        .network_connected = true,
        .network_supported = true,
        .network_has_config = true,
        .network_details_valid = true,
        .network_ssid = "Home Wi-Fi",
        .network_ip = "192.168.1.42",
        .network_gateway = "192.168.1.1",
        .network_netmask = "255.255.255.0",
        .network_mac = "10:51:DB:80:E4:54",
        .network_bssid = "02:11:22:33:44:55",
        .network_rssi = -58,
        .network_channel = 6,
        .time_valid = true,
        .time_text = "--:--",
    };
    s_boot_short_presses = 0;
}

void mock_backend_handle_event(szpi_ui_event_t event, uint32_t value, void *context)
{
    (void)context;
    if (event == SZPI_UI_EVENT_BRIGHTNESS_CHANGED && value <= 100) s_model.display_brightness_percent = (uint8_t)(value < 10 ? 10 : value);
    if (event == SZPI_UI_EVENT_SPEAKER_VOLUME_CHANGED && value <= 100) s_model.speaker_volume_percent = (uint8_t)value;
    if (event == SZPI_UI_EVENT_MIC_GAIN_CHANGED && value <= 100) s_model.microphone_gain_percent = (uint8_t)value;
    if (event == SZPI_UI_EVENT_AUDIO_TEST_TONE) s_model.audio_state = SZPI_UI_AUDIO_COMPLETE;
    if (event == SZPI_UI_EVENT_AUDIO_TEST_CAPTURE) {
        s_model.audio_state = SZPI_UI_AUDIO_COMPLETE;
        s_model.audio_blocks_processed = 100;
        s_model.audio_peak_sample = 0;
        s_model.audio_rms_sample = 0;
    }
    if (event == SZPI_UI_EVENT_AUDIO_STOP) s_model.audio_state = SZPI_UI_AUDIO_IDLE;
    if (event == SZPI_UI_EVENT_STORAGE_RETRY) s_model.storage_state = SZPI_UI_STORAGE_READY;
    if (event == SZPI_UI_EVENT_STORAGE_FORMAT && value == s_model.storage_generation) {
        s_model.storage_state = SZPI_UI_STORAGE_READY;
        s_model.storage_free_bytes = s_model.storage_capacity_bytes;
        s_model.storage_fat_type = 3;
        s_model.storage_generation++;
    }
    if (event == SZPI_UI_EVENT_PRIMARY_ACTION) s_model.click_count++;
}

void mock_backend_get_state(szpi_ui_model_t *model)
{
    if (model == NULL) return;
    *model = s_model;
    time_t now = time(NULL);
    struct tm local_time;
    if (now >= (time_t)1704067200 && localtime_r(&now, &local_time) != NULL) {
        (void)strftime(model->time_text, sizeof(model->time_text), "%H:%M", &local_time);
    }
}

uint32_t mock_backend_boot_short_press(void)
{
    return ++s_boot_short_presses;
}
