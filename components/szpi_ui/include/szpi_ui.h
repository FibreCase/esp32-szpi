#ifndef SZPI_UI_H
#define SZPI_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef enum {
    SZPI_UI_EVENT_PRIMARY_ACTION = 1,
} szpi_ui_event_t;

typedef enum {
    SZPI_UI_RESULT_OK = 0,
    SZPI_UI_RESULT_INVALID_ARGUMENT,
    SZPI_UI_RESULT_INVALID_STATE,
    SZPI_UI_RESULT_NO_MEMORY,
} szpi_ui_result_t;

typedef struct {
    uint32_t click_count;
    uint32_t imu_sequence;
    float accel_g[3];
    bool imu_available;
    bool imu_valid;
    bool network_connected;
    bool time_valid;
    char time_text[6];
} szpi_ui_model_t;

typedef void (*szpi_ui_event_cb_t)(szpi_ui_event_t event, void *context);

/*
 * All functions must be called from the thread that owns LVGL.
 * create() requires lv_init() and a registered display, and returns INVALID_STATE
 * if called before destroy(). update() copies the supplied state. destroy() is
 * idempotent and deletes the UI objects, not the LVGL display. Event callbacks
 * run inline on the LVGL thread and must not perform blocking work.
 */
szpi_ui_result_t szpi_ui_create(szpi_ui_event_cb_t event_cb, void *context);
szpi_ui_result_t szpi_ui_update(const szpi_ui_model_t *model);
void szpi_ui_destroy(void);

#endif
