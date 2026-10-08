#ifndef SZPI_SIMULATOR_MOCK_BACKEND_H
#define SZPI_SIMULATOR_MOCK_BACKEND_H

#include "szpi_ui.h"

void mock_backend_init(void);
void mock_backend_handle_event(szpi_ui_event_t event, uint32_t value, void *context);
void mock_backend_get_state(szpi_ui_model_t *model);
uint32_t mock_backend_boot_short_press(void);

#endif
