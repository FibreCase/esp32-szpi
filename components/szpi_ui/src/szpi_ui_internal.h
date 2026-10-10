#ifndef SZPI_UI_INTERNAL_H
#define SZPI_UI_INTERNAL_H

#include "szpi_ui.h"

typedef enum {
    SZPI_UI_MENU_NETWORK = 0,
    SZPI_UI_MENU_DISPLAY,
    SZPI_UI_MENU_CAMERA,
    SZPI_UI_MENU_AUDIO,
    SZPI_UI_MENU_STORAGE,
    SZPI_UI_MENU_ABOUT,
    SZPI_UI_MENU_COUNT,
} szpi_ui_menu_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *network_dot;
    lv_obj_t *network_label;
    lv_obj_t *title_label;
    lv_obj_t *time_label;
    bool network_state_set;
    bool network_connected;
    char displayed_time[6];
} szpi_ui_page_t;

bool szpi_ui_page_create(szpi_ui_page_t *page, const char *title, bool show_status,
                         const char *parent_title);
void szpi_ui_style_card(lv_obj_t *card);
void szpi_ui_screen_gesture_event(lv_event_t *event);
void szpi_ui_emit(szpi_ui_event_t event, uint32_t value);

bool szpi_ui_display_test_create(lv_obj_t *panel, lv_obj_t *parent_screen);
void szpi_ui_display_test_update(const szpi_ui_model_t *model);
void szpi_ui_display_test_destroy(void);

bool szpi_ui_camera_test_create(lv_obj_t *panel, lv_obj_t *parent_screen);
void szpi_ui_camera_test_update(const szpi_ui_model_t *model);
void szpi_ui_camera_test_destroy(void);

bool szpi_ui_network_create(lv_obj_t *panel, lv_obj_t *parent_screen);
bool szpi_ui_network_update(const szpi_ui_model_t *model);
void szpi_ui_network_destroy(void);

bool szpi_ui_storage_create(lv_obj_t *panel);
void szpi_ui_storage_update(const szpi_ui_model_t *model);
void szpi_ui_storage_cancel_confirmation(void);
void szpi_ui_storage_destroy(void);

#endif
