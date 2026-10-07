#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SZPI_BOOT_DEBOUNCE_MS 20U
#define SZPI_BOOT_LONG_PRESS_MS 800U

typedef struct {
    uint32_t candidate_since_ms;
    uint32_t press_started_ms;
    bool candidate_pressed;
    bool stable_pressed;
    bool armed;
    bool long_press_reported;
} szpi_boot_debouncer_t;

typedef enum {
    SZPI_BOOT_STATE_EVENT_NONE = 0,
    SZPI_BOOT_STATE_EVENT_PRESSED = 1U << 0,
    SZPI_BOOT_STATE_EVENT_LONG_PRESS = 1U << 1,
    SZPI_BOOT_STATE_EVENT_RELEASED = 1U << 2,
    SZPI_BOOT_STATE_EVENT_SHORT_PRESS = 1U << 3,
} szpi_boot_state_event_t;

static inline void szpi_boot_debouncer_seed(szpi_boot_debouncer_t *state,
                                            bool initially_pressed,
                                            uint32_t now_ms)
{
    *state = (szpi_boot_debouncer_t){
        .candidate_since_ms = now_ms,
        .press_started_ms = now_ms,
        .candidate_pressed = initially_pressed,
        .stable_pressed = initially_pressed,
        .armed = !initially_pressed,
    };
}

static inline uint32_t szpi_boot_debouncer_step(szpi_boot_debouncer_t *state,
                                                bool raw_pressed,
                                                uint32_t now_ms)
{
    uint32_t events = SZPI_BOOT_STATE_EVENT_NONE;
    if (raw_pressed != state->candidate_pressed) {
        state->candidate_pressed = raw_pressed;
        state->candidate_since_ms = now_ms;
    }

    uint32_t candidate_age_ms = now_ms - state->candidate_since_ms;
    if (state->candidate_pressed != state->stable_pressed &&
        candidate_age_ms >= SZPI_BOOT_DEBOUNCE_MS) {
        state->stable_pressed = state->candidate_pressed;
        if (!state->armed) {
            // A key held during boot/re-enable must first be stably released.
            if (!state->stable_pressed) state->armed = true;
            state->long_press_reported = false;
        } else if (state->stable_pressed) {
            state->press_started_ms = state->candidate_since_ms + SZPI_BOOT_DEBOUNCE_MS;
            state->long_press_reported = false;
            events |= SZPI_BOOT_STATE_EVENT_PRESSED;
        } else {
            events |= SZPI_BOOT_STATE_EVENT_RELEASED;
            if (!state->long_press_reported) events |= SZPI_BOOT_STATE_EVENT_SHORT_PRESS;
            state->long_press_reported = false;
        }
    }

    if (state->armed && state->stable_pressed && !state->long_press_reported &&
        (uint32_t)(now_ms - state->press_started_ms) >= SZPI_BOOT_LONG_PRESS_MS) {
        state->long_press_reported = true;
        events |= SZPI_BOOT_STATE_EVENT_LONG_PRESS;
    }
    return events;
}
