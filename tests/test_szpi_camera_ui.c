/* Headless lifecycle and staging-source checks for the shared camera page. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../components/szpi_ui/src/szpi_ui.c"
#include "../components/szpi_ui/src/szpi_ui_camera_test.c"

static uint16_t s_test_pixels[320 * 240];
static uint16_t s_test_draw[320 * 40];
static unsigned s_starts;
static unsigned s_stops;
static unsigned s_flushes;
static uint16_t s_rendered_pixels[320 * 240];

static void test_event(szpi_ui_event_t event, uint32_t value, void *context)
{
    (void)context;
    assert(value == 0);
    if (event == SZPI_UI_EVENT_CAMERA_TEST_START) s_starts++;
    if (event == SZPI_UI_EVENT_CAMERA_TEST_STOP) s_stops++;
}

static void test_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const uint16_t *source = (const uint16_t *)pixels;
    for (int y = area->y1; y <= area->y2; ++y) {
        size_t width = area->x2 - area->x1 + 1;
        memcpy(s_rendered_pixels + y * 320 + area->x1, source, width * sizeof(*source));
        source += width;
    }
    s_flushes++;
    lv_display_flush_ready(display);
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(320, 240);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_test_draw, NULL, sizeof(s_test_draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, test_flush);
    assert(szpi_ui_create(test_event, NULL) == SZPI_UI_RESULT_OK);
    lv_image_dsc_t source = {
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                   .w = 320, .h = 240, .stride = 640},
        .data_size = sizeof(s_test_pixels), .data = (const uint8_t *)s_test_pixels,
    };
    assert(szpi_ui_camera_test_set_frame(&source) == SZPI_UI_RESULT_INVALID_STATE);
    for (unsigned i = 0; i < 10; ++i) {
        lv_screen_load(s_camera_test_page.screen);
        assert(s_starts == i + 1 && s_stops == i);
        lv_image_dsc_t invalid = source;
        invalid.data_size--;
        assert(szpi_ui_camera_test_set_frame(&invalid) == SZPI_UI_RESULT_INVALID_ARGUMENT);
        for (size_t p = 0; p < 320 * 240; ++p) s_test_pixels[p] = p % 320 < 160 ? 0xF800 : 0x001F;
        assert(szpi_ui_camera_test_set_frame(&source) == SZPI_UI_RESULT_OK);
        assert(!lv_obj_has_flag(s_camera_image, LV_OBJ_FLAG_HIDDEN));
        szpi_ui_model_t model = {.camera_state = SZPI_UI_CAMERA_RUNNING, .camera_fps_milli = 12345};
        assert(szpi_ui_update(&model) == SZPI_UI_RESULT_OK);
        assert(strstr(lv_label_get_text(s_camera_status), "FPS 12.3"));
        lv_refr_now(display);
        assert(s_flushes > 0);
        assert(s_rendered_pixels[100 * 320 + 40] == 0xF800);
        assert(s_rendered_pixels[100 * 320 + 240] == 0x001F);
        for (size_t p = 0; p < 320 * 240; ++p) s_test_pixels[p] = 0x07E0;
        assert(szpi_ui_camera_test_set_frame(&source) == SZPI_UI_RESULT_OK);
        lv_refr_now(display);
        assert(s_rendered_pixels[100 * 320 + 40] == 0x07E0);
        model.camera_state = SZPI_UI_CAMERA_FAULT;
        model.camera_error_code = 0x107;
        szpi_ui_update(&model);
        assert(strstr(lv_label_get_text(s_camera_status), "Camera error"));
        assert(strstr(lv_label_get_text(s_camera_status), "00000107"));
        lv_screen_load(s_detail_pages[SZPI_UI_MENU_CAMERA].screen);
        assert(s_stops == i + 1);
        assert(szpi_ui_camera_test_set_frame(&source) == SZPI_UI_RESULT_INVALID_STATE);
        assert(szpi_ui_camera_test_set_frame(NULL) == SZPI_UI_RESULT_OK);
    }
    lv_screen_load(s_camera_test_page.screen);
    szpi_ui_destroy();
    assert(s_starts == s_stops);
    assert(szpi_ui_camera_test_set_frame(NULL) == SZPI_UI_RESULT_INVALID_STATE);
    assert(szpi_ui_create(test_event, NULL) == SZPI_UI_RESULT_OK);
    lv_screen_load(s_camera_test_page.screen);
    szpi_ui_model_t offline = {.camera_state = SZPI_UI_CAMERA_UNAVAILABLE};
    szpi_ui_update(&offline);
    assert(strstr(lv_label_get_text(s_camera_status), "Camera unavailable"));
    assert(lv_obj_has_flag(s_camera_image, LV_OBJ_FLAG_HIDDEN));
    szpi_ui_destroy();
    assert(s_starts == s_stops);
    lv_display_delete(display);
    lv_deinit();
    puts("Camera UI lifecycle and staging source: PASS");
}
