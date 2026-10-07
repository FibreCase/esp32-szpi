#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t touch_count;
    uint8_t primary_id;
    bool pressed;
    bool faulted;
    uint32_t consecutive_errors;
    uint32_t read_duration_us;
} szpi_input_state_t;

esp_err_t szpi_input_init(void);
// Reads and maps one stable primary point. No touch returns a released state.
esp_err_t szpi_input_read(szpi_input_state_t *state);
esp_err_t szpi_input_deinit(void);
