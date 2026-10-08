#include "mock_backend.h"

#include <time.h>

static szpi_ui_model_t s_model;
static uint32_t s_boot_short_presses;

void mock_backend_init(void)
{
    s_model = (szpi_ui_model_t){
        .imu_sequence = 1,
        .accel_g = {0.0f, 0.0f, 1.0f},
        .imu_available = true,
        .imu_valid = true,
        .network_connected = true,
        .time_valid = true,
        .time_text = "--:--",
    };
    s_boot_short_presses = 0;
}

void mock_backend_handle_event(szpi_ui_event_t event, void *context)
{
    (void)context;
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
