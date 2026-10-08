#include "szpi_ui.h"

#include <string.h>

extern const lv_font_t szpi_ui_font_button;

enum {
    SZPI_UI_MENU_DISPLAY = 0,
    SZPI_UI_MENU_NETWORK,
    SZPI_UI_MENU_AUDIO,
    SZPI_UI_MENU_STORAGE,
    SZPI_UI_MENU_ABOUT,
    SZPI_UI_MENU_COUNT,
};

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

typedef struct {
    szpi_ui_page_t page;
    lv_obj_t *preview_bar;
    lv_obj_t *preview_value;
    lv_obj_t *editor_value;
} szpi_ui_slider_test_t;

static szpi_ui_slider_test_t s_slider_tests[2];

static szpi_ui_page_t s_home_page;
static szpi_ui_page_t s_settings_page;
static szpi_ui_page_t s_detail_pages[SZPI_UI_MENU_COUNT];
static lv_obj_t *s_settings_menu_panel;
static bool s_settings_active;
static int s_active_detail = -1;

static void screen_gesture_event(lv_event_t *event);
static void menu_item_event(lv_event_t *event);
static void return_to_settings(void);

static void style_card(lv_obj_t *card)
{
    lv_obj_remove_style_all(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_NONE);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x15191F), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x303844), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static bool create_page(szpi_ui_page_t *page, const char *title, bool show_status,
                        const char *parent_title)
{
    page->screen = lv_obj_create(NULL);
    if (page->screen == NULL) return false;
    lv_obj_remove_style_all(page->screen);
    lv_obj_remove_flag(page->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page->screen, LV_DIR_NONE);
    lv_obj_set_style_text_font(page->screen, &szpi_ui_font_button, 0);
    lv_obj_set_style_bg_color(page->screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(page->screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(page->screen, screen_gesture_event, LV_EVENT_GESTURE, NULL);

    page->title_label = lv_label_create(page->screen);
    if (page->title_label == NULL) return false;
    lv_label_set_text(page->title_label, title);
    lv_obj_set_style_text_color(page->title_label, lv_color_hex(0xE7EAF0), 0);
    lv_obj_align(page->title_label, LV_ALIGN_TOP_MID, 0, 4);
    if (parent_title != NULL) {
        lv_obj_t *parent_label = lv_label_create(page->screen);
        if (parent_label == NULL) return false;
        lv_label_set_text_fmt(parent_label, "< %s", parent_title);
        lv_obj_set_style_text_color(parent_label, lv_color_hex(0x8B95A5), 0);
        lv_obj_align(parent_label, LV_ALIGN_TOP_LEFT, 12, 4);
        lv_obj_remove_flag(parent_label, LV_OBJ_FLAG_CLICKABLE);
    }
    if (!show_status) return true;

    page->network_dot = lv_obj_create(page->screen);
    if (page->network_dot == NULL) return false;
    lv_obj_remove_style_all(page->network_dot);
    lv_obj_set_size(page->network_dot, 7, 7);
    lv_obj_set_style_radius(page->network_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(page->network_dot, LV_OPA_COVER, 0);
    lv_obj_align(page->network_dot, LV_ALIGN_TOP_LEFT, 12, 10);

    page->network_label = lv_label_create(page->screen);
    if (page->network_label == NULL) return false;
    lv_label_set_text(page->network_label, "Offline");
    lv_obj_set_style_text_color(page->network_label, lv_color_hex(0x8B95A5), 0);
    lv_obj_align(page->network_label, LV_ALIGN_TOP_LEFT, 25, 4);

    page->time_label = lv_label_create(page->screen);
    if (page->time_label == NULL) return false;
    lv_label_set_text(page->time_label, "--:--");
    lv_obj_set_style_text_color(page->time_label, lv_color_hex(0xAAB2BF), 0);
    lv_obj_align(page->time_label, LV_ALIGN_TOP_RIGHT, -12, 4);
    return true;
}

static bool create_settings_menu(void)
{
    static const char *const menu_items[SZPI_UI_MENU_COUNT] = {
        "Display",
        "Network",
        "Audio",
        "Storage",
        "About",
    };
    lv_obj_t *panel = lv_obj_create(s_settings_page.screen);
    if (panel == NULL) return false;
    s_settings_menu_panel = panel;
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 304, 212);
    lv_obj_set_pos(panel, 8, 28);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_pad_bottom(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);

    for (size_t i = 0; i < SZPI_UI_MENU_COUNT; ++i) {
        lv_obj_t *row = lv_obj_create(panel);
        if (row == NULL) return false;
        style_card(row);
        lv_obj_set_size(row, lv_pct(100), 60);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, menu_item_event, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)(i + 1));

        lv_obj_t *label = lv_label_create(row);
        if (label == NULL) return false;
        lv_label_set_text(label, menu_items[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(0xD7DCE5), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    return true;
}

static bool create_display_row(lv_obj_t *panel, lv_coord_t y,
                               const char *title, const char *value)
{
    lv_obj_t *row = lv_obj_create(panel);
    if (row == NULL) return false;
    style_card(row);
    lv_obj_set_size(row, lv_pct(100), 56);
    lv_obj_set_pos(row, 0, y);

    lv_obj_t *title_label = lv_label_create(row);
    lv_obj_t *value_label = lv_label_create(row);
    if (title_label == NULL || value_label == NULL) return false;
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xD7DCE5), 0);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 12, 0);
    lv_label_set_text(value_label, value);
    lv_obj_set_style_text_color(value_label, lv_color_hex(0x8B95A5), 0);
    lv_obj_align(value_label, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_flag(title_label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(value_label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return true;
}

static bool create_switch_row(lv_obj_t *panel, lv_coord_t y, const char *title,
                              bool initial_state)
{
    lv_obj_t *row = lv_obj_create(panel);
    if (row == NULL) return false;
    style_card(row);
    lv_obj_set_size(row, lv_pct(100), 56);
    lv_obj_set_pos(row, 0, y);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_t *toggle = lv_switch_create(row);
    if (label == NULL || toggle == NULL) return false;
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(0xD7DCE5), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_size(toggle, 62, 36);
    lv_obj_set_ext_click_area(toggle, 6);
    lv_obj_set_style_bg_color(toggle, lv_color_hex(0x343C48), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(toggle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(toggle, lv_color_hex(0x34D399), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -12, 0);
    if (initial_state) lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(toggle, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return true;
}

static void slider_value_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *slider = lv_event_get_target(event);
    szpi_ui_slider_test_t *test = lv_event_get_user_data(event);
    int value = (int)lv_slider_get_value(slider);
    lv_label_set_text_fmt(test->editor_value, "%d%%", value);
    lv_label_set_text_fmt(test->preview_value, "%d%%", value);
    lv_bar_set_value(test->preview_bar, value, LV_ANIM_OFF);
}

static void slider_card_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) return;
    szpi_ui_slider_test_t *test = lv_event_get_user_data(event);
    lv_screen_load_anim(test->page.screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

static void slider_back_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_screen_load_anim(s_detail_pages[SZPI_UI_MENU_AUDIO].screen,
                        LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
}

static bool create_slider_row(lv_obj_t *panel, lv_coord_t y, const char *title,
                              int initial_value, size_t index)
{
    szpi_ui_slider_test_t *test = &s_slider_tests[index];
    lv_obj_t *row = lv_obj_create(panel);
    if (row == NULL) return false;
    style_card(row);
    lv_obj_set_size(row, lv_pct(100), 78);
    lv_obj_set_pos(row, 0, y);

    lv_obj_t *title_label = lv_label_create(row);
    lv_obj_t *value_label = lv_label_create(row);
    lv_obj_t *bar = lv_bar_create(row);
    if (title_label == NULL || value_label == NULL || bar == NULL) return false;
    test->preview_bar = bar;
    test->preview_value = value_label;
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xD7DCE5), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 12, 8);
    lv_label_set_text_fmt(value_label, "%d%%", initial_value);
    lv_obj_set_style_text_color(value_label, lv_color_hex(0xAAB2BF), 0);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, initial_value, LV_ANIM_OFF);
    lv_obj_set_size(bar, lv_pct(90), 8);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x343C48), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x34D399), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, slider_card_event, LV_EVENT_CLICKED, test);
    lv_obj_add_flag(title_label, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(value_label, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    if (!create_page(&test->page, title, false, "Audio")) return false;
    /* Horizontal movement here belongs exclusively to the slider. */
    lv_obj_remove_event_cb(test->page.screen, screen_gesture_event);
    test->editor_value = lv_label_create(test->page.screen);
    lv_obj_t *slider = lv_slider_create(test->page.screen);
    lv_obj_t *back = lv_button_create(test->page.screen);
    if (test->editor_value == NULL || slider == NULL || back == NULL) return false;
    lv_label_set_text_fmt(test->editor_value, "%d%%", initial_value);
    lv_obj_set_style_text_color(test->editor_value, lv_color_hex(0xE7EAF0), 0);
    lv_obj_align(test->editor_value, LV_ALIGN_TOP_MID, 0, 60);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, initial_value, LV_ANIM_OFF);
    lv_obj_set_size(slider, 264, 20);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x343C48), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x34D399), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xE7EAF0), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 12);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(slider, slider_value_event, LV_EVENT_VALUE_CHANGED, test);
    style_card(back);
    lv_obj_set_size(back, 144, 44);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x252D38), LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, slider_back_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    if (back_label == NULL) return false;
    lv_label_set_text(back_label, "Back");
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xE7EAF0), 0);
    lv_obj_center(back_label);
    return true;
}

static bool create_detail_page(size_t index, const char *title)
{
    szpi_ui_page_t *page = &s_detail_pages[index];
    if (!create_page(page, title, false, "Settings")) return false;

    lv_obj_t *panel = lv_obj_create(page->screen);
    if (panel == NULL) return false;
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 304, 210);
    lv_obj_set_pos(panel, 8, 30);
    lv_obj_set_style_pad_bottom(panel, 8, 0);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);

    switch (index) {
        case SZPI_UI_MENU_DISPLAY:
            return create_display_row(panel, 0, "Screen", "320 x 240") &&
                   create_display_row(panel, 64, "Theme", "Dark") &&
                   create_display_row(panel, 128, "Orientation", "Auto");
        case SZPI_UI_MENU_NETWORK:
            return create_switch_row(panel, 0, "Wi-Fi", true) &&
                   create_switch_row(panel, 64, "Auto reconnect", false) &&
                   create_display_row(panel, 128, "Connection", "Test mode");
        case SZPI_UI_MENU_AUDIO:
            return create_slider_row(panel, 0, "Speaker level", 68, 0) &&
                   create_slider_row(panel, 88, "Microphone gain", 42, 1);
        case SZPI_UI_MENU_STORAGE:
            return create_switch_row(panel, 0, "Loop recording", false) &&
                   create_switch_row(panel, 64, "Auto save", true) &&
                   create_display_row(panel, 128, "Card", "Not connected");
        case SZPI_UI_MENU_ABOUT:
            return create_display_row(panel, 0, "Device", "SZ-PI") &&
                   create_display_row(panel, 64, "Interface", "Test menu") &&
                   create_display_row(panel, 128, "Version", "Prototype");
        default:
            return false;
    }
}

static void screen_gesture_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) return;

    lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
    lv_obj_t *screen = lv_event_get_current_target(event);
    bool handled = false;
    if (screen == s_home_page.screen && direction == LV_DIR_LEFT && !s_settings_active) {
        s_settings_active = true;
        lv_obj_scroll_to_y(s_settings_menu_panel, 0, LV_ANIM_OFF);
        lv_screen_load_anim(s_settings_page.screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
        handled = true;
    } else if (screen == s_settings_page.screen && s_settings_active &&
               (direction == LV_DIR_LEFT || direction == LV_DIR_RIGHT)) {
        if (direction == LV_DIR_RIGHT) {
            s_settings_active = false;
            lv_screen_load_anim(s_home_page.screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
        }
        /* Consume forward swipes so release cannot become a menu click. */
        handled = true;
    } else {
        for (size_t i = 0; i < SZPI_UI_MENU_COUNT; ++i) {
            if (screen == s_detail_pages[i].screen && s_active_detail == (int)i &&
                direction == LV_DIR_RIGHT) {
                return_to_settings();
                handled = true;
                break;
            }
        }
    }

    if (handled) lv_indev_wait_release(lv_indev_active());
}

static void menu_item_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !s_settings_active) return;
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) return;
    uintptr_t encoded_index = (uintptr_t)lv_event_get_user_data(event);
    if (encoded_index == 0 || encoded_index > SZPI_UI_MENU_COUNT) return;
    size_t index = (size_t)(encoded_index - 1);
    s_settings_active = false;
    s_active_detail = (int)index;
    lv_screen_load_anim(s_detail_pages[index].screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

static void return_to_settings(void)
{
    if (s_active_detail < 0 || !s_detail_pages[s_active_detail].screen) return;
    s_active_detail = -1;
    s_settings_active = true;
    lv_screen_load_anim(s_settings_page.screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
}

static void update_page_status(szpi_ui_page_t *page, const szpi_ui_model_t *model,
                               const char *time_text)
{
    if (!page->network_state_set || page->network_connected != model->network_connected) {
        page->network_state_set = true;
        page->network_connected = model->network_connected;
        lv_obj_set_style_bg_color(page->network_dot,
            model->network_connected ? lv_color_hex(0x34D399) : lv_color_hex(0x5B6472), 0);
        lv_label_set_text(page->network_label, model->network_connected ? "Wi-Fi" : "Offline");
    }
    if (strcmp(page->displayed_time, time_text) != 0) {
        lv_label_set_text(page->time_label, time_text);
        memcpy(page->displayed_time, time_text, sizeof(page->displayed_time));
    }
}

szpi_ui_result_t szpi_ui_create(szpi_ui_event_cb_t event_cb, void *context)
{
    if (s_home_page.screen != NULL || s_settings_page.screen != NULL) {
        return SZPI_UI_RESULT_INVALID_STATE;
    }
    (void)event_cb;
    (void)context;
    s_home_page = (szpi_ui_page_t){0};
    s_settings_page = (szpi_ui_page_t){0};
    memset(s_detail_pages, 0, sizeof(s_detail_pages));
    s_settings_menu_panel = NULL;
    s_settings_active = false;
    s_active_detail = -1;

    if (!create_page(&s_home_page, "SZ-PI", true, NULL) ||
        !create_page(&s_settings_page, "Settings", false, "Home") ||
        !create_settings_menu()) {
        szpi_ui_destroy();
        return SZPI_UI_RESULT_NO_MEMORY;
    }
    static const char *const detail_titles[SZPI_UI_MENU_COUNT] = {
        "Display", "Network", "Audio", "Storage", "About",
    };
    for (size_t i = 0; i < SZPI_UI_MENU_COUNT; ++i) {
        if (!create_detail_page(i, detail_titles[i])) {
            szpi_ui_destroy();
            return SZPI_UI_RESULT_NO_MEMORY;
        }
    }

    lv_screen_load(s_home_page.screen);
    return SZPI_UI_RESULT_OK;
}

szpi_ui_result_t szpi_ui_update(const szpi_ui_model_t *model)
{
    if (model == NULL) return SZPI_UI_RESULT_INVALID_ARGUMENT;
    if (s_home_page.screen == NULL || s_settings_page.screen == NULL) {
        return SZPI_UI_RESULT_INVALID_STATE;
    }

    char time_text[sizeof(model->time_text)];
    if (model->time_valid) memcpy(time_text, model->time_text, sizeof(time_text));
    else memcpy(time_text, "--:--", sizeof(time_text));
    time_text[sizeof(time_text) - 1] = '\0';

    update_page_status(&s_home_page, model, time_text);
    return SZPI_UI_RESULT_OK;
}

void szpi_ui_destroy(void)
{
    if (s_home_page.screen != NULL) lv_screen_load(s_home_page.screen);
    for (size_t i = 0; i < 2; ++i) {
        if (s_slider_tests[i].page.screen != NULL) lv_obj_delete(s_slider_tests[i].page.screen);
    }
    memset(s_slider_tests, 0, sizeof(s_slider_tests));
    for (size_t i = 0; i < SZPI_UI_MENU_COUNT; ++i) {
        if (s_detail_pages[i].screen != NULL) lv_obj_delete(s_detail_pages[i].screen);
    }
    if (s_settings_page.screen != NULL) lv_obj_delete(s_settings_page.screen);
    if (s_home_page.screen != NULL) lv_obj_delete(s_home_page.screen);
    s_home_page = (szpi_ui_page_t){0};
    s_settings_page = (szpi_ui_page_t){0};
    memset(s_detail_pages, 0, sizeof(s_detail_pages));
    s_settings_menu_panel = NULL;
    s_settings_active = false;
    s_active_detail = -1;
}
