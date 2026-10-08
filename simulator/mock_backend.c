#include "mock_backend.h"

static szpi_ui_model_t s_model;
static uint32_t s_boot_short_presses;

void mock_backend_init(void)
{
    s_model = (szpi_ui_model_t){
        .imu_sequence = 1,
        .accel_g = {0.0f, 0.0f, 1.0f},
        .imu_available = true,
        .imu_valid = true,
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
    if (model != NULL) *model = s_model;
}

uint32_t mock_backend_boot_short_press(void)
{
    return ++s_boot_short_presses;
}
