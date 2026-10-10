#include "szpi_ui_internal.h"

#include <string.h>
#include "src/misc/cache/instance/lv_image_cache.h"

static szpi_ui_page_t s_camera_test_page;
static lv_obj_t *s_camera_image;
static lv_obj_t *s_camera_status;
static bool s_camera_test_active;
static char s_camera_status_text[96];

static void camera_test_lifecycle(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_SCREEN_LOADED) {
        s_camera_test_active = true;
        lv_obj_add_flag(s_camera_image, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_camera_status, "Starting camera...");
        s_camera_status_text[0] = '\0';
        szpi_ui_emit(SZPI_UI_EVENT_CAMERA_TEST_START, 0);
    } else if (lv_event_get_code(event) == LV_EVENT_SCREEN_UNLOAD_START && s_camera_test_active) {
        s_camera_test_active = false;
        szpi_ui_emit(SZPI_UI_EVENT_CAMERA_TEST_STOP, 0);
    }
}

static void camera_test_open(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) return;
    lv_screen_load_anim(s_camera_test_page.screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

static void camera_test_gesture(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL || lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) return;
    lv_screen_load_anim(lv_event_get_user_data(event), LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
    lv_indev_wait_release(indev);
}

bool szpi_ui_camera_test_create(lv_obj_t *panel, lv_obj_t *parent_screen)
{
    lv_obj_t *row = lv_obj_create(panel);
    if (row == NULL) return false;
    szpi_ui_style_card(row);
    lv_obj_set_size(row, lv_pct(100), 56);
    lv_obj_set_pos(row, 0, 192);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, camera_test_open, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(row);
    if (label == NULL) return false;
    lv_label_set_text(label, "Test");
    lv_obj_set_style_text_color(label, lv_color_hex(0xD7DCE5), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    if (!szpi_ui_page_create(&s_camera_test_page, "Test", false, "Camera")) return false;
    lv_obj_remove_event_cb(s_camera_test_page.screen, szpi_ui_screen_gesture_event);
    lv_obj_add_event_cb(s_camera_test_page.screen, camera_test_gesture, LV_EVENT_GESTURE, parent_screen);
    s_camera_image = lv_image_create(s_camera_test_page.screen);
    if (s_camera_image == NULL) return false;
    lv_obj_set_pos(s_camera_image, 0, 0);
    lv_obj_add_flag(s_camera_image, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_move_to_index(s_camera_image, 0);
    /* Opaque overlays retain navigation and stats above the full QVGA frame. */
    lv_obj_t *header = lv_obj_create(s_camera_test_page.screen);
    if (header == NULL) return false;
    lv_obj_remove_style_all(header);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(header, 320, 28);
    lv_obj_set_style_bg_color(header, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_add_flag(header, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_move_to_index(header, 1);
    s_camera_status = lv_label_create(s_camera_test_page.screen);
    if (s_camera_status == NULL) return false;
    lv_obj_set_size(s_camera_status, 320, 48);
    lv_obj_set_pos(s_camera_status, 0, 192);
    lv_obj_set_style_text_color(s_camera_status, lv_color_hex(0xD7DCE5), 0);
    lv_obj_set_style_bg_color(s_camera_status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_camera_status, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_camera_status, 8, 0);
    lv_obj_add_flag(s_camera_status, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_label_set_text(s_camera_status, "Starting camera...");
    lv_obj_add_event_cb(s_camera_test_page.screen, camera_test_lifecycle, LV_EVENT_ALL, NULL);
    return true;
}

szpi_ui_result_t szpi_ui_camera_test_set_frame(const lv_image_dsc_t *source)
{
    if (s_camera_image == NULL) return SZPI_UI_RESULT_INVALID_STATE;
    if (source == NULL) {
        lv_obj_add_flag(s_camera_image, LV_OBJ_FLAG_HIDDEN);
        const void *previous = lv_image_get_src(s_camera_image);
        lv_image_set_src(s_camera_image, NULL);
        if (previous != NULL) lv_image_cache_drop(previous);
        return SZPI_UI_RESULT_OK;
    }
    if (!s_camera_test_active) return SZPI_UI_RESULT_INVALID_STATE;
    if (source->data == NULL || source->header.w != 320 || source->header.h != 240 ||
        source->header.cf != LV_COLOR_FORMAT_RGB565 || source->header.stride != 640 ||
        source->data_size != 320 * 240 * 2) return SZPI_UI_RESULT_INVALID_ARGUMENT;
    lv_image_cache_drop(source);
    lv_image_set_src(s_camera_image, source);
    lv_obj_remove_flag(s_camera_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_camera_image);
    return SZPI_UI_RESULT_OK;
}

void szpi_ui_camera_test_update(const szpi_ui_model_t *model)
{
    if (!s_camera_test_active || s_camera_status == NULL) return;
    const char *state = "Camera unavailable";
    switch (model->camera_state) {
        case SZPI_UI_CAMERA_STOPPED: state = "Waiting for camera"; break;
        case SZPI_UI_CAMERA_STARTING: state = "Starting camera..."; break;
        case SZPI_UI_CAMERA_RUNNING: state = "Live preview"; break;
        case SZPI_UI_CAMERA_STOPPING: state = "Stopping camera..."; break;
        case SZPI_UI_CAMERA_FAULT: state = "Camera error"; break;
        default: break;
    }
    char text[sizeof(s_camera_status_text)];
    lv_snprintf(text, sizeof(text), "%s  FPS %lu.%lu\nErrors %lu  Code %08lX", state,
        (unsigned long)(model->camera_fps_milli / 1000),
        (unsigned long)(model->camera_fps_milli % 1000 / 100),
        (unsigned long)model->camera_error_count, (unsigned long)model->camera_error_code);
    if (strcmp(text, s_camera_status_text) != 0) {
        lv_label_set_text(s_camera_status, text);
        memcpy(s_camera_status_text, text, strlen(text) + 1);
    }
}

void szpi_ui_camera_test_destroy(void)
{
    if (s_camera_test_active) szpi_ui_emit(SZPI_UI_EVENT_CAMERA_TEST_STOP, 0);
    s_camera_test_active = false;
    if (s_camera_test_page.screen != NULL) lv_obj_delete(s_camera_test_page.screen);
    s_camera_test_page = (szpi_ui_page_t){0};
    s_camera_image = NULL;
    s_camera_status = NULL;
    s_camera_status_text[0] = '\0';
}
