#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SZPI_INPUT_RAW_X_MAX 239U
#define SZPI_INPUT_RAW_Y_MAX 319U
#define SZPI_INPUT_DISPLAY_WIDTH 320U
#define SZPI_INPUT_DISPLAY_HEIGHT 240U
#define SZPI_INPUT_EVENT_LIFT_UP 1U
#define SZPI_INPUT_EVENT_NO_EVENT 3U

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t id;
    uint8_t event;
} szpi_input_raw_touch_t;

static inline bool szpi_input_touch_event_is_active(uint8_t event)
{
    return event != SZPI_INPUT_EVENT_LIFT_UP && event != SZPI_INPUT_EVENT_NO_EVENT;
}

static inline int szpi_input_select_primary(const szpi_input_raw_touch_t *points, uint8_t count,
                                            bool has_previous, uint8_t previous_id)
{
    if (points == NULL) return -1;
    if (has_previous) {
        for (uint8_t i = 0; i < count; ++i) {
            if (points[i].id == previous_id && szpi_input_touch_event_is_active(points[i].event)) return i;
        }
    }
    for (uint8_t i = 0; i < count; ++i) {
        if (szpi_input_touch_event_is_active(points[i].event)) return i;
    }
    return -1;
}

static inline bool szpi_input_map_coordinates(uint16_t raw_x, uint16_t raw_y,
                                              uint16_t *x, uint16_t *y)
{
    if (x == NULL || y == NULL || raw_x > SZPI_INPUT_RAW_X_MAX || raw_y > SZPI_INPUT_RAW_Y_MAX) {
        return false;
    }
    uint16_t mapped_x = (SZPI_INPUT_DISPLAY_WIDTH - 1U) - raw_y;
    uint16_t mapped_y = raw_x;
    if (mapped_x >= SZPI_INPUT_DISPLAY_WIDTH || mapped_y >= SZPI_INPUT_DISPLAY_HEIGHT) return false;
    *x = mapped_x;
    *y = mapped_y;
    return true;
}
