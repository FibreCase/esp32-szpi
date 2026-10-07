#include "driver/gpio.h"
#include "esp_timer.h"
#include "szpi_board.h"
#include "szpi_input.h"
#include "szpi_boot_state.h"

static const szpi_board_bindings_t *s_bindings;
static szpi_boot_debouncer_t s_debouncer;
static bool s_initialized;

esp_err_t szpi_boot_button_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t err = szpi_board_get_bindings(&s_bindings);
    if (err != ESP_OK) return err;
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << s_bindings->boot_button_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&config);
    if (err != ESP_OK) {
        s_bindings = NULL;
        return err;
    }

    int level = gpio_get_level(s_bindings->boot_button_gpio);
    bool pressed = s_bindings->boot_button_active_low ? level == 0 : level != 0;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    szpi_boot_debouncer_seed(&s_debouncer, pressed, now_ms);
    s_initialized = true;
    return ESP_OK;
}

esp_err_t szpi_boot_button_read(szpi_boot_state_t *state)
{
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || s_bindings == NULL) return ESP_ERR_INVALID_STATE;
    int level = gpio_get_level(s_bindings->boot_button_gpio);
    bool pressed = s_bindings->boot_button_active_low ? level == 0 : level != 0;
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint32_t events = szpi_boot_debouncer_step(&s_debouncer, pressed,
        (uint32_t)(now_us / 1000ULL));
    *state = (szpi_boot_state_t){
        .events = (szpi_boot_event_t)events,
        .timestamp_us = now_us,
        .pressed = s_debouncer.stable_pressed,
    };
    return ESP_OK;
}

esp_err_t szpi_boot_button_deinit(void)
{
    if (!s_initialized || s_bindings == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t err = gpio_reset_pin(s_bindings->boot_button_gpio);
    s_initialized = false;
    s_bindings = NULL;
    s_debouncer = (szpi_boot_debouncer_t){0};
    return err;
}
