#include "szpi_ui_internal.h"

#include <string.h>

static szpi_ui_page_t s_network_info_page, s_network_method_page;
static szpi_ui_page_t s_network_qr_pages[2];
#define s_network_qr_page s_network_qr_pages[s_network_dpp ? 0 : 1]
#define s_network_qr s_network_qrs[s_network_dpp ? 0 : 1]
#define s_network_qr_text s_network_qr_texts[s_network_dpp ? 0 : 1]
#define s_network_qr_payload s_network_qr_payloads[s_network_dpp ? 0 : 1]
static lv_obj_t *s_network_saved_label, *s_network_info_label, *s_network_message;
static lv_obj_t *s_network_qrs[2], *s_network_qr_texts[2], *s_network_forget_button;
static szpi_ui_model_t s_network_model;
static char s_network_qr_payloads[2][512];
static uint32_t s_network_request_counter, s_network_page_request;
static bool s_network_dpp, s_network_forget_confirm, s_network_boot_notice;
static lv_obj_t *s_network_parent_screen;

static void network_emit(szpi_ui_event_t event)
{
    szpi_ui_emit(event, event == SZPI_UI_EVENT_NETWORK_BEGIN ? ((s_network_page_request << 1) | (s_network_dpp ? 1U : 0U)) :
        (event == SZPI_UI_EVENT_NETWORK_CANCEL ? s_network_page_request : s_network_model.provisioning_generation));
}

static void network_click(lv_event_t *event)
{
    uintptr_t action = (uintptr_t)lv_event_get_user_data(event);
    if (action == 1) {
        s_network_forget_confirm = false;
        lv_obj_remove_state(s_network_forget_button, LV_STATE_DISABLED);
        lv_label_set_text(lv_obj_get_child(s_network_forget_button, 0), "Clear Wi-Fi settings");
        lv_screen_load_anim(s_network_info_page.screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
    } else if (action == 2) {
        lv_screen_load_anim(s_network_method_page.screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
    } else if (action == 3 || action == 4) {
        s_network_dpp = action == 3;
        lv_label_set_text(s_network_qr_page.title_label, s_network_dpp ? "Easy Connect" : "Wi-Fi Hotspot");
        s_network_qr_payload[0] = 0;
        lv_obj_add_flag(s_network_qr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_network_qr_text, "Preparing setup...");
        lv_screen_load_anim(s_network_qr_page.screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
    } else if (action == 5) {
        if (!s_network_model.network_supported || !s_network_model.network_has_config || s_network_model.provisioning_active) return;
        if (!s_network_forget_confirm) {
            s_network_forget_confirm = true;
            lv_label_set_text(lv_obj_get_child(s_network_forget_button, 0), "Confirm clear settings");
        } else {
            s_network_forget_confirm = false;
            network_emit(SZPI_UI_EVENT_NETWORK_FORGET);
            lv_obj_add_state(s_network_forget_button, LV_STATE_DISABLED);
        }

    }
}

static lv_obj_t *network_button(lv_obj_t *parent, int x, int y, int width,
                               const char *title, uintptr_t action)
{
    lv_obj_t *button = lv_button_create(parent);
    if (!button) return NULL;
    szpi_ui_style_card(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 48);
    lv_obj_t *label = lv_label_create(button);
    if (!label) return NULL;
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE7EAF0), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, network_click, LV_EVENT_CLICKED, (void *)action);
    return button;
}

static void network_gesture(lv_event_t *event)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_RIGHT) return;
    lv_obj_t *screen = lv_event_get_current_target(event);
    if (screen == s_network_qr_page.screen) {
        lv_screen_load_anim(s_network_method_page.screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
    } else {
        lv_screen_load_anim(s_network_parent_screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
    }
    lv_indev_wait_release(lv_indev_active());
}

static void network_qr_lifecycle(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_SCREEN_LOADED) {
        s_network_dpp = lv_event_get_current_target(event) == s_network_qr_pages[0].screen;
        s_network_request_counter = (s_network_request_counter + 1) & 0x7FFFFFFFU;
        if (!s_network_request_counter) s_network_request_counter = 1;
        s_network_page_request = s_network_request_counter;
        network_emit(SZPI_UI_EVENT_NETWORK_BEGIN);
    } else if (code == LV_EVENT_SCREEN_UNLOAD_START && s_network_page_request) {
        /* This also cancels a queued begin before its status reaches the UI. */
        network_emit(SZPI_UI_EVENT_NETWORK_CANCEL);
        s_network_page_request = 0;
    }
}

bool szpi_ui_network_create(lv_obj_t *panel, lv_obj_t *parent_screen)
{
    s_network_parent_screen = parent_screen;
    lv_obj_t *saved = network_button(panel, 0, 0, 304, "", 1);
    if (!saved || !network_button(panel, 0, 60, 304, "Connect a New Network", 2)) return false;
    s_network_saved_label = lv_obj_get_child(saved, 0);
    lv_label_set_text(s_network_saved_label, "No saved network");
    s_network_message = lv_label_create(panel);
    if (!s_network_message) return false;
    lv_obj_set_pos(s_network_message, 12, 122);
    lv_obj_set_width(s_network_message, 280);
    lv_obj_set_style_text_color(s_network_message, lv_color_hex(0xAAB2BF), 0);
    lv_label_set_text(s_network_message, "");
    szpi_ui_page_t *pages[] = {&s_network_info_page, &s_network_method_page, &s_network_qr_pages[0], &s_network_qr_pages[1]};
    const char *titles[] = {"Current Wi-Fi", "Connect", "Easy Connect", "Wi-Fi Hotspot"};
    for (unsigned i = 0; i < 4; i++) {
        if (!szpi_ui_page_create(pages[i], titles[i], false, "Network")) return false;
        lv_obj_remove_event_cb(pages[i]->screen, szpi_ui_screen_gesture_event);
        lv_obj_add_event_cb(pages[i]->screen, network_gesture, LV_EVENT_GESTURE, NULL);
    }
    lv_obj_t *info_panel = lv_obj_create(s_network_info_page.screen);
    if (!info_panel) return false;
    lv_obj_remove_style_all(info_panel);
    lv_obj_set_pos(info_panel, 8, 32);
    lv_obj_set_size(info_panel, 304, 208);
    lv_obj_set_style_pad_all(info_panel, 8, 0);
    lv_obj_set_style_pad_row(info_panel, 16, 0);
    lv_obj_set_flex_flow(info_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(info_panel, LV_DIR_VER);
    lv_obj_add_flag(info_panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
    s_network_info_label = lv_label_create(info_panel);
    if (!s_network_info_label) return false;
    lv_obj_set_pos(s_network_info_label, 16, 44);
    lv_obj_set_width(s_network_info_label, 288);
    lv_obj_set_style_text_color(s_network_info_label, lv_color_hex(0xD7DCE5), 0);
    s_network_forget_button = network_button(info_panel, 0, 0, 288, "Clear Wi-Fi settings", 5);
    if (!s_network_forget_button || !network_button(s_network_method_page.screen, 8, 42, 304, "Easy Connect (DPP)", 3) ||
        !network_button(s_network_method_page.screen, 8, 104, 304, "Wi-Fi Hotspot", 4)) return false;
    lv_obj_t *hint = lv_label_create(s_network_method_page.screen);
    if (!hint) return false;
    lv_obj_set_width(hint, 288);
    lv_obj_set_pos(hint, 16, 168);
    lv_label_set_text(hint, "Use Hotspot if your phone does not support Easy Connect.");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xAAB2BF), 0);
    for (unsigned i = 0; i < 2; i++) {
        s_network_dpp = i == 0;
        s_network_qr = lv_qrcode_create(s_network_qr_page.screen);
        s_network_qr_text = lv_label_create(s_network_qr_page.screen);
        if (!s_network_qr || !s_network_qr_text) return false;
        lv_qrcode_set_size(s_network_qr, s_network_dpp ? 196 : 192);
        lv_qrcode_set_dark_color(s_network_qr, lv_color_black());
        lv_qrcode_set_light_color(s_network_qr, lv_color_white());
        lv_qrcode_set_quiet_zone(s_network_qr, true);
        lv_obj_set_pos(s_network_qr, s_network_dpp ? 62 : 4, 32);
        lv_obj_set_pos(s_network_qr_text, s_network_dpp ? 12 : 204, s_network_dpp ? 54 : 34);
        lv_obj_set_width(s_network_qr_text, s_network_dpp ? 296 : 112);
        lv_obj_set_style_text_color(s_network_qr_text, lv_color_hex(0xD7DCE5), 0);
        lv_obj_add_flag(s_network_qr, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(s_network_qr_text, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(s_network_qr_page.screen, network_qr_lifecycle, LV_EVENT_ALL, NULL);
    }
    return true;
}

bool szpi_ui_network_update(const szpi_ui_model_t *model)
{
    s_network_model = *model;
    bool open_setup = !s_network_boot_notice && model->network_supported &&
                      model->network_needs_setup && !model->network_has_config;
    if (open_setup) s_network_boot_notice = true;
    const char *ssid = model->network_has_config ? model->network_ssid : "No saved network";
    lv_label_set_text(s_network_saved_label, ssid);
    char signal[32] = "--", channel[8] = "--";
    bool details = model->network_connected && model->network_details_valid;
    if (details) {
        lv_snprintf(signal, sizeof(signal), "%d dBm (%s)", model->network_rssi,
            model->network_rssi >= -60 ? "Strong" : model->network_rssi >= -75 ? "Fair" : "Weak");
        lv_snprintf(channel, sizeof(channel), "%u", model->network_channel);
    }
    lv_label_set_text_fmt(s_network_info_label,
        "SSID: %s\nStatus: %s\nIP: %s\nGateway: %s\nSubnet: %s\nDevice MAC: %s\nAP MAC: %s\nSignal: %s\nChannel: %s",
        ssid, model->network_connected ? "Connected" : "Offline",
        details ? model->network_ip : "--", details ? model->network_gateway : "--",
        details ? model->network_netmask : "--", model->network_mac[0] ? model->network_mac : "--",
        details ? model->network_bssid : "--", signal, channel);
    if (!model->network_supported || !model->network_has_config || model->provisioning_active) lv_obj_add_state(s_network_forget_button, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_network_forget_button, LV_STATE_DISABLED);
    lv_label_set_text(s_network_message, model->network_supported ? model->provisioning_message : "Preview only. Wi-Fi setup runs on device.");
    const char *payload = s_network_dpp ? model->setup_dpp_uri : model->setup_wifi_qr;
    bool qr_ready = model->provisioning_active && model->provisioning_state != 4 && model->provisioning_state != 2 &&
        payload[0] && (!s_network_dpp || model->dpp_ready);
    if (qr_ready && strcmp(s_network_qr_payload, payload)) {
        if (lv_qrcode_update(s_network_qr, payload, strlen(payload)) == LV_RESULT_OK) {
            lv_snprintf(s_network_qr_payload, sizeof(s_network_qr_payload), "%s", payload);
        } else { qr_ready = false; s_network_qr_payload[0] = 0; }
    }
    if (s_network_dpp && qr_ready) lv_obj_add_flag(s_network_qr_text, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_network_qr_text, LV_OBJ_FLAG_HIDDEN);
    if (qr_ready) lv_obj_remove_flag(s_network_qr, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_network_qr, LV_OBJ_FLAG_HIDDEN);
    if (!model->network_supported) lv_label_set_text(s_network_qr_text, "Preview only\nUse the device for setup.");
    else if (!model->provisioning_active || model->provisioning_state == 2 || model->provisioning_state == 4 || model->provisioning_state == 5) {
        lv_label_set_text(s_network_qr_text, model->provisioning_message[0] ? model->provisioning_message : "Setup closed. Return to start again.");
    } else if (s_network_dpp) {
        lv_label_set_text(s_network_qr_text, model->dpp_ready ? "Scan with an Easy Connect compatible phone.\n\nNo support? Use Wi-Fi Hotspot." : "Easy Connect unavailable. Return and use Wi-Fi Hotspot.");
    } else {
        lv_label_set_text_fmt(s_network_qr_text, "SSID\n%s\n\nPassword\n%s\n\n192.168.4.1", model->setup_ssid, model->setup_password);
    }
    return open_setup;
}

void szpi_ui_network_destroy(void)
{
    if (s_network_page_request) network_emit(SZPI_UI_EVENT_NETWORK_CANCEL);
    s_network_page_request = 0;
    szpi_ui_page_t *pages[] = {&s_network_info_page, &s_network_method_page,
                               &s_network_qr_pages[0], &s_network_qr_pages[1]};
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        if (pages[i]->screen != NULL) lv_obj_delete(pages[i]->screen);
        *pages[i] = (szpi_ui_page_t){0};
    }
    memset(&s_network_model, 0, sizeof(s_network_model));
    memset(s_network_qr_payloads, 0, sizeof(s_network_qr_payloads));
    memset(s_network_qrs, 0, sizeof(s_network_qrs));
    memset(s_network_qr_texts, 0, sizeof(s_network_qr_texts));
    s_network_saved_label = NULL;
    s_network_info_label = NULL;
    s_network_message = NULL;
    s_network_forget_button = NULL;
    s_network_parent_screen = NULL;
    s_network_dpp = false;
    s_network_forget_confirm = false;
    s_network_boot_notice = false;
}
