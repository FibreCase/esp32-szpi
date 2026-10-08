/* Headless rendering exercises shared pages within the same LVGL pool as device. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../components/szpi_ui/src/szpi_ui.c"
static uint16_t frame[320 * 240], draw[320 * 40];
static unsigned begin_count, cancel_count;
static uint32_t last_begin, last_cancel;
static void ui_event(szpi_ui_event_t event, uint32_t value, void *context)
{
    (void)context;
    if (event == SZPI_UI_EVENT_NETWORK_BEGIN) { begin_count++; last_begin = value; }
    if (event == SZPI_UI_EVENT_NETWORK_CANCEL) { cancel_count++; last_cancel = value; }
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const uint16_t *source = (const uint16_t *)pixels;
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(frame + y * 320 + area->x1, source, (area->x2 - area->x1 + 1) * 2);
        source += area->x2 - area->x1 + 1;
    }
    lv_display_flush_ready(display);
}
static void render(lv_display_t *display, const char *filename)
{
    lv_obj_update_layout(lv_screen_active());
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    FILE *file = fopen(filename, "wb");
    assert(file);
    fprintf(file, "P6\n320 240\n255\n");
    for (unsigned i = 0; i < 320 * 240; i++) {
        uint16_t color = frame[i];
        uint8_t rgb[] = {(color >> 11) * 255 / 31, ((color >> 5) & 63) * 255 / 63, (color & 31) * 255 / 31};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}
int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(320, 240);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw, NULL, sizeof(draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    assert(szpi_ui_create(ui_event, NULL) == SZPI_UI_RESULT_OK);
    szpi_ui_model_t model = {.network_supported = true, .network_has_config = true, .network_connected = true,
        .provisioning_active = true, .provisioning_state = 1, .dpp_ready = true, .provisioning_generation = 1,
        .display_brightness_percent = 50, .network_ssid = "Home Wi-Fi", .setup_ssid = "Device-AABBCC",
        .setup_password = "abcdefgh12345678", .setup_wifi_qr = "WIFI:T:WPA;S:Device-AABBCC;P:abcdefgh12345678;;",
        .provisioning_message = "Choose Easy Connect or Wi-Fi Hotspot."};
    assert(szpi_ui_update(&model) == SZPI_UI_RESULT_OK);
    lv_screen_load(s_detail_pages[SZPI_UI_MENU_NETWORK].screen);
    render(display, "/tmp/szpi-network.ppm");
    model.network_details_valid = true;
    model.network_rssi = -58;
    model.network_channel = 6;
    strcpy(model.network_ip, "192.168.1.42");
    strcpy(model.network_gateway, "192.168.1.1");
    strcpy(model.network_netmask, "255.255.255.0");
    strcpy(model.network_mac, "10:51:DB:80:E4:54");
    strcpy(model.network_bssid, "02:11:22:33:44:55");
    szpi_ui_update(&model);
    lv_screen_load(s_network_info_page.screen);
    render(display, "/tmp/szpi-current-wifi.ppm");
    assert(strstr(lv_label_get_text(s_network_info_label), "192.168.1.42"));
    assert(strstr(lv_label_get_text(s_network_info_label), "-58 dBm"));
    lv_obj_t *info_panel = lv_obj_get_parent(s_network_info_label);
    assert(lv_obj_get_scroll_bottom(info_panel) > 0);
    model.network_connected = false;
    szpi_ui_update(&model);
    assert(!strstr(lv_label_get_text(s_network_info_label), "192.168.1.42"));
    assert(strstr(lv_label_get_text(s_network_info_label), "IP: --"));
    model.network_connected = true;
    szpi_ui_update(&model);
    lv_screen_load(s_network_method_page.screen);
    render(display, "/tmp/szpi-method.ppm");
    s_network_dpp = false;
    lv_screen_load(s_network_qr_page.screen);
    szpi_ui_update(&model);
    assert(!lv_obj_has_flag(s_network_qr, LV_OBJ_FLAG_HIDDEN));
    render(display, "/tmp/szpi-hotspot.ppm");
    assert(lv_obj_get_width(s_network_qr) == 192);
    assert(begin_count == 1 && (last_begin & 1U) == 0);
    uint32_t hotspot_request = last_begin >> 1;
    strcpy(model.setup_dpp_uri, "DPP:C:81/6;M:AABBCCDDEEFF;I:SZ-PI;K:MDkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDIgAC7yK2ihVCjX61Z0ybPkAGnrXMTT_XujLG-jUt-XkUXP4;;");
    s_network_dpp = true;
    lv_screen_load(s_network_qr_page.screen);
    szpi_ui_update(&model);
    assert(!lv_obj_has_flag(s_network_qr, LV_OBJ_FLAG_HIDDEN));
    lv_label_set_text(s_network_qr_page.title_label, "Easy Connect");
    render(display, "/tmp/szpi-dpp.ppm");
    assert(lv_obj_get_width(s_network_qr) == 196);
    assert(begin_count == 2 && (last_begin & 1U) == 1);
    assert(cancel_count == 1 && last_cancel == hotspot_request);
    for (unsigned i = 0; i < lv_obj_get_child_count(s_network_qr_page.screen); i++) {
        assert(!lv_obj_check_type(lv_obj_get_child(s_network_qr_page.screen, i), &lv_button_class));
    }
    model.provisioning_state = 2;
    szpi_ui_update(&model);
    assert(lv_obj_has_flag(s_network_qr, LV_OBJ_FLAG_HIDDEN));
    szpi_ui_destroy();
    assert(szpi_ui_create(NULL, NULL) == SZPI_UI_RESULT_OK);
    szpi_ui_destroy();
    lv_display_delete(display);
    lv_deinit();
    puts("Network UI and QR rendering: PASS");
}
