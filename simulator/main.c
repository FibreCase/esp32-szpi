#define SDL_MAIN_HANDLED

#include <stdbool.h>
#include <stdint.h>
#include <SDL2/SDL.h>

#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_keyboard.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include "mock_backend.h"

typedef struct {
    bool running;
} simulator_context_t;

static int SDLCALL simulator_event_filter(void *userdata, SDL_Event *event)
{
    simulator_context_t *context = userdata;
    if (event->type == SDL_QUIT ||
        (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_CLOSE)) {
        context->running = false;
        return 0;
    }
    if (event->type != SDL_KEYDOWN) return 1;

    switch (event->key.keysym.sym) {
        case SDLK_ESCAPE:
            context->running = false;
            return 0;
        case SDLK_b:
            SDL_Log("mock BOOT short press: %u", (unsigned)mock_backend_boot_short_press());
            return 0;
        default:
            return 1;
    }
}

int main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return 1;
    lv_init();

    lv_display_t *display = lv_sdl_window_create(320, 240);
    if (display == NULL) {
        SDL_Quit();
        return 1;
    }
    lv_sdl_window_set_title(display, "SZPI UI Simulator");
    lv_sdl_window_set_resizeable(display, false);
    lv_sdl_window_set_zoom(display, 1.0f);
    mock_backend_init();
    lv_indev_t *mouse = lv_sdl_mouse_create();
    lv_indev_t *keyboard = lv_sdl_keyboard_create();
    if (mouse == NULL || keyboard == NULL) {
        lv_sdl_quit();
        SDL_Quit();
        return 1;
    }

    if (szpi_ui_create(mock_backend_handle_event, NULL) != SZPI_UI_RESULT_OK) {
        lv_indev_delete(mouse);
        lv_indev_delete(keyboard);
        lv_sdl_quit();
        lv_deinit();
        SDL_Quit();
        return 1;
    }
    simulator_context_t context = {
        .running = true,
    };
    SDL_SetEventFilter(simulator_event_filter, &context);
    while (context.running) {
        szpi_ui_model_t model;
        mock_backend_get_state(&model);
        (void)szpi_ui_update(&model);
        (void)lv_timer_handler();
        SDL_Delay(5);
    }

    SDL_SetEventFilter(NULL, NULL);
    szpi_ui_destroy();
    lv_indev_delete(mouse);
    lv_indev_delete(keyboard);
    lv_sdl_quit();
    lv_deinit();
    SDL_Quit();
    return 0;
}
