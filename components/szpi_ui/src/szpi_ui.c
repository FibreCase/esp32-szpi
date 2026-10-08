#include "szpi_ui.h"

extern const lv_font_t szpi_ui_font_button;

static lv_obj_t *s_screen;
static lv_obj_t *s_button;
static lv_obj_t *s_button_label;
static lv_obj_t *s_imu_label;
static szpi_ui_event_cb_t s_event_cb;
static void *s_event_context;
static uint32_t s_click_count;
static uint32_t s_imu_sequence;
static bool s_imu_available;
static bool s_imu_valid;
static bool s_imu_state_set;

static void primary_action_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    if (s_event_cb != NULL) s_event_cb(SZPI_UI_EVENT_PRIMARY_ACTION, s_event_context);
}

szpi_ui_result_t szpi_ui_create(szpi_ui_event_cb_t event_cb, void *context)
{
    if (s_screen != NULL) return SZPI_UI_RESULT_INVALID_STATE;
    s_event_cb = event_cb;
    s_event_context = context;
    s_click_count = 0;
    s_imu_sequence = 0;
    s_imu_available = false;
    s_imu_valid = false;
    s_imu_state_set = false;
    s_screen = lv_obj_create(NULL);
    if (s_screen == NULL) goto no_memory;
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_text_font(s_screen, &szpi_ui_font_button, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0xF3F5F8), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    s_imu_label = lv_label_create(s_screen);
    if (s_imu_label == NULL) goto no_memory;
    lv_label_set_text(s_imu_label, "IMU waiting");
    lv_obj_set_style_text_color(s_imu_label, lv_color_hex(0x334155), 0);
    lv_obj_align(s_imu_label, LV_ALIGN_TOP_MID, 0, 28);

    s_button = lv_button_create(s_screen);
    if (s_button == NULL) goto no_memory;
    lv_obj_set_size(s_button, 144, 56);
    lv_obj_center(s_button);
    lv_obj_set_style_radius(s_button, 12, 0);
    lv_obj_set_style_bg_color(s_button, lv_color_hex(0x2563EB), 0);
    lv_obj_set_style_bg_color(s_button, lv_color_hex(0x1D4ED8), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(s_button, 0, 0);
    lv_obj_add_event_cb(s_button, primary_action_event, LV_EVENT_CLICKED, NULL);

    s_button_label = lv_label_create(s_button);
    if (s_button_label == NULL) goto no_memory;
    lv_label_set_text(s_button_label, "Button");
    lv_obj_set_style_text_color(s_button_label, lv_color_white(), 0);
    lv_obj_center(s_button_label);

    lv_screen_load(s_screen);
    return SZPI_UI_RESULT_OK;

no_memory:
    szpi_ui_destroy();
    return SZPI_UI_RESULT_NO_MEMORY;
}

szpi_ui_result_t szpi_ui_update(const szpi_ui_model_t *model)
{
    if (model == NULL) return SZPI_UI_RESULT_INVALID_ARGUMENT;
    if (s_button_label == NULL || s_imu_label == NULL) return SZPI_UI_RESULT_INVALID_STATE;
    if (model->click_count != s_click_count) {
        s_click_count = model->click_count;
        lv_label_set_text_fmt(s_button_label, "Clicks: %lu", (unsigned long)s_click_count);
    }
    if (!s_imu_state_set || model->imu_available != s_imu_available || model->imu_valid != s_imu_valid ||
        (model->imu_valid && model->imu_sequence != s_imu_sequence)) {
        s_imu_state_set = true;
        s_imu_available = model->imu_available;
        s_imu_valid = model->imu_valid;
        s_imu_sequence = model->imu_sequence;
        if (!s_imu_available) {
            lv_label_set_text(s_imu_label, "IMU unavailable");
        } else if (!s_imu_valid) {
            lv_label_set_text(s_imu_label, "IMU waiting");
        } else {
            int32_t values[3];
            uint32_t magnitudes[3];
            for (size_t axis = 0; axis < 3; ++axis) {
                values[axis] = (int32_t)(model->accel_g[axis] * 100.0f +
                    (model->accel_g[axis] >= 0.0f ? 0.5f : -0.5f));
                magnitudes[axis] = (uint32_t)(values[axis] < 0 ? -values[axis] : values[axis]);
            }
            lv_label_set_text_fmt(s_imu_label,
                "X %c%lu.%02lug  Y %c%lu.%02lug  Z %c%lu.%02lug",
                values[0] < 0 ? '-' : '+', (unsigned long)(magnitudes[0] / 100U), (unsigned long)(magnitudes[0] % 100U),
                values[1] < 0 ? '-' : '+', (unsigned long)(magnitudes[1] / 100U), (unsigned long)(magnitudes[1] % 100U),
                values[2] < 0 ? '-' : '+', (unsigned long)(magnitudes[2] / 100U), (unsigned long)(magnitudes[2] % 100U));
        }
    }
    return SZPI_UI_RESULT_OK;
}

void szpi_ui_destroy(void)
{
    if (s_screen != NULL) lv_obj_delete(s_screen);
    s_screen = NULL;
    s_button = NULL;
    s_button_label = NULL;
    s_imu_label = NULL;
    s_event_cb = NULL;
    s_event_context = NULL;
    s_click_count = 0;
    s_imu_sequence = 0;
    s_imu_available = false;
    s_imu_valid = false;
    s_imu_state_set = false;
}
