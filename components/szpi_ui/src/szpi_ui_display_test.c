#include "szpi_ui_internal.h"

#include <string.h>

static szpi_ui_page_t s_display_test_page;
static lv_timer_t *s_display_test_timer;
static lv_obj_t *s_display_test_area;
static lv_obj_t *s_display_test_stripes[10];
static lv_obj_t *s_display_test_stats_label;
static uint32_t s_display_test_start_tick;
static bool s_display_test_running;
static char s_display_test_stats_text[128];

static void display_test_tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t phase = (lv_tick_elaps(s_display_test_start_tick) / 8) % 80;
    for (size_t i = 0; i < 10; ++i) {
        lv_obj_set_x(s_display_test_stripes[i], (int32_t)i * 40 - (int32_t)phase);
    }
    /* Force the entire test area to change, rather than measuring tiny updates. */
    lv_obj_invalidate(s_display_test_area);
    /* The animation and refresh timers can run in either order. Make the
     * updated image eligible at the next handler instead of another period. */
    lv_timer_t *refresh_timer = lv_display_get_refr_timer(lv_obj_get_display(s_display_test_area));
    if (refresh_timer != NULL) lv_timer_ready(refresh_timer);
}

static void display_test_lifecycle(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_SCREEN_LOADED) {
        s_display_test_running = true;
        s_display_test_start_tick = lv_tick_get();
        s_display_test_stats_text[0] = '\0';
        lv_timer_resume(s_display_test_timer);
        lv_timer_ready(s_display_test_timer);
        szpi_ui_emit(SZPI_UI_EVENT_DISPLAY_TEST_START, 0);
    } else if (lv_event_get_code(event) == LV_EVENT_SCREEN_UNLOAD_START && s_display_test_running) {
        s_display_test_running = false;
        lv_timer_pause(s_display_test_timer);
        szpi_ui_emit(SZPI_UI_EVENT_DISPLAY_TEST_STOP, 0);
    }
}

static void display_test_open(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) return;
    lv_screen_load_anim(s_display_test_page.screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

static void display_test_gesture(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE ||
        lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_RIGHT) return;
    lv_screen_load_anim(lv_event_get_user_data(event), LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
    lv_indev_wait_release(lv_indev_active());
}

bool szpi_ui_display_test_create(lv_obj_t *panel, lv_obj_t *parent_screen)
{
    lv_obj_t *row = lv_obj_create(panel);
    if (row == NULL) return false;
    szpi_ui_style_card(row);
    lv_obj_set_size(row, lv_pct(100), 56);
    lv_obj_set_pos(row, 0, 278);
    lv_obj_add_event_cb(row, display_test_open, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *label = lv_label_create(row);
    if (label == NULL) return false;
    lv_label_set_text(label, "FPS / Tearing");
    lv_obj_set_style_text_color(label, lv_color_hex(0xD7DCE5), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    if (!szpi_ui_page_create(&s_display_test_page, "Test", false, "Display")) return false;
    lv_obj_remove_event_cb(s_display_test_page.screen, szpi_ui_screen_gesture_event);
    lv_obj_add_event_cb(s_display_test_page.screen, display_test_gesture, LV_EVENT_GESTURE, parent_screen);
    s_display_test_area = lv_obj_create(s_display_test_page.screen);
    if (s_display_test_area == NULL) return false;
    lv_obj_remove_style_all(s_display_test_area);
    lv_obj_remove_flag(s_display_test_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_display_test_area, 320, 240);
    lv_obj_set_pos(s_display_test_area, 0, 0);
    lv_obj_set_style_bg_color(s_display_test_area, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_display_test_area, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_display_test_area, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_move_to_index(s_display_test_area, 0);
    lv_obj_t *header = lv_obj_create(s_display_test_page.screen);
    if (header == NULL) return false;
    lv_obj_remove_style_all(header);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(header, 320, 28);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_add_flag(header, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_move_to_index(header, 1);
    for (size_t i = 0; i < 10; ++i) {
        lv_obj_t *stripe = lv_obj_create(s_display_test_area);
        if (stripe == NULL) return false;
        s_display_test_stripes[i] = stripe;
        lv_obj_remove_style_all(stripe);
        lv_obj_remove_flag(stripe, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(stripe, 40, 240);
        lv_obj_set_pos(stripe, (int32_t)i * 40, 0);
        lv_obj_set_style_bg_color(stripe, i % 2 ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);
        lv_obj_add_flag(stripe, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    s_display_test_stats_label = lv_label_create(s_display_test_page.screen);
    if (s_display_test_stats_label == NULL) return false;
    lv_label_set_text(s_display_test_stats_label, "Measuring...");
    lv_obj_set_style_text_color(s_display_test_stats_label, lv_color_hex(0xAAB2BF), 0);
    lv_obj_set_style_bg_color(s_display_test_stats_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_display_test_stats_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_display_test_stats_label, 8, 0);
    lv_obj_set_style_pad_top(s_display_test_stats_label, 4, 0);
    lv_obj_set_size(s_display_test_stats_label, 320, 56);
    lv_obj_set_pos(s_display_test_stats_label, 0, 184);
    s_display_test_timer = lv_timer_create(display_test_tick, 16, NULL);
    if (s_display_test_timer == NULL) return false;
    lv_timer_pause(s_display_test_timer);
    lv_obj_add_event_cb(s_display_test_page.screen, display_test_lifecycle, LV_EVENT_ALL, NULL);
    return true;
}

void szpi_ui_display_test_update(const szpi_ui_model_t *model)
{
    if (!s_display_test_running || s_display_test_stats_label == NULL) return;
    char text[sizeof(s_display_test_stats_text)];
    if (!model->display_test_supported) {
        lv_snprintf(text, sizeof(text), "Desktop preview\nHardware stats on device");
    } else if (!model->display_test_valid) {
        lv_snprintf(text, sizeof(text), "Measuring...");
    } else {
        lv_snprintf(text, sizeof(text), "FPS %lu.%lu  TX %lu.%lu/%lu.%lums\nLVGL %lu.%lu  Gap %lu.%lums",
            (unsigned long)(model->display_fps_x10 / 10), (unsigned long)(model->display_fps_x10 % 10),
            (unsigned long)(model->display_frame_avg_us / 1000), (unsigned long)(model->display_frame_avg_us % 1000 / 100),
            (unsigned long)(model->display_frame_max_us / 1000), (unsigned long)(model->display_frame_max_us % 1000 / 100),
            (unsigned long)(model->display_lvgl_avg_us / 1000), (unsigned long)(model->display_lvgl_avg_us % 1000 / 100),
            (unsigned long)(model->display_gap_avg_us / 1000), (unsigned long)(model->display_gap_avg_us % 1000 / 100));
    }
    if (strcmp(text, s_display_test_stats_text) != 0) {
        lv_label_set_text(s_display_test_stats_label, text);
        memcpy(s_display_test_stats_text, text, strlen(text) + 1);
    }
}

void szpi_ui_display_test_destroy(void)
{
    if (s_display_test_running) szpi_ui_emit(SZPI_UI_EVENT_DISPLAY_TEST_STOP, 0);
    s_display_test_running = false;
    if (s_display_test_timer != NULL) lv_timer_delete(s_display_test_timer);
    s_display_test_timer = NULL;
    if (s_display_test_page.screen != NULL) lv_obj_delete(s_display_test_page.screen);
    s_display_test_page = (szpi_ui_page_t){0};
    s_display_test_area = NULL;
    s_display_test_stats_label = NULL;
    memset(s_display_test_stripes, 0, sizeof(s_display_test_stripes));
    s_display_test_stats_text[0] = '\0';
}
