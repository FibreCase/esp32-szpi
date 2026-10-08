#include "szpi_ui_internal.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *s_status_value;
static lv_obj_t *s_capacity_value;
static lv_obj_t *s_free_value;
static lv_obj_t *s_filesystem_value;
static lv_obj_t *s_speed_value;
static lv_obj_t *s_hint;
static lv_obj_t *s_retry_button;
static lv_obj_t *s_format_button;
static lv_obj_t *s_format_label;
static uint32_t s_confirmation_generation;
static bool s_confirming_format;
static bool s_initialized;
static bool s_displayed_state_valid;
static szpi_ui_storage_state_t s_displayed_state;

static void set_label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static lv_obj_t *storage_label(lv_obj_t *parent, const char *text, lv_coord_t x,
                               lv_coord_t y, lv_coord_t width, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) return NULL;
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return label;
}

static lv_obj_t *storage_button(lv_obj_t *parent, const char *text,
                                lv_coord_t x, lv_coord_t width, lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    if (button == NULL) return NULL;
    lv_obj_set_size(button, width, 36);
    lv_obj_set_pos(button, x, 174);
    lv_obj_set_style_radius(button, 9, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x242B35), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(0x394554), LV_PART_MAIN);
    lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    if (label == NULL) return NULL;
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE7EAF0), 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    return button;
}

static const char *storage_state_text(szpi_ui_storage_state_t state)
{
    switch (state) {
        case SZPI_UI_STORAGE_NO_CARD: return "No SD card";
        case SZPI_UI_STORAGE_CARD_READY_NO_FS: return "No filesystem";
        case SZPI_UI_STORAGE_READY: return "Ready";
        case SZPI_UI_STORAGE_BUSY: return "Storage busy";
        case SZPI_UI_STORAGE_FORMATTING: return "Formatting FAT32";
        case SZPI_UI_STORAGE_FAULT: return "Storage error";
        case SZPI_UI_STORAGE_UNINITIALIZED:
        default: return "Checking SD card";
    }
}

static void format_size(uint64_t bytes, char *text, size_t capacity)
{
    static const uint64_t unit = 1024ULL * 1024ULL * 1024ULL;
    if (bytes >= unit) {
        (void)snprintf(text, capacity, "%llu.%llu GB",
            (unsigned long long)(bytes / unit),
            (unsigned long long)((bytes % unit) * 10ULL / unit));
    } else {
        (void)snprintf(text, capacity, "%llu MB",
            (unsigned long long)(bytes / (1024ULL * 1024ULL)));
    }
}

static const char *filesystem_text(const szpi_ui_model_t *model)
{
    if (model->storage_state == SZPI_UI_STORAGE_CARD_READY_NO_FS) return "Unformatted";
    if (model->storage_fat_type == 3) return "FAT32";
    if (model->storage_fat_type == 2) return "FAT16";
    if (model->storage_fat_type == 1) return "FAT12";
    return model->storage_state == SZPI_UI_STORAGE_READY ? "Unknown" : "--";
}

static void retry_clicked(lv_event_t *event)
{
    (void)event;
    szpi_ui_emit(SZPI_UI_EVENT_STORAGE_RETRY, 0);
}

static void format_clicked(lv_event_t *event)
{
    (void)event;
    if (s_format_button == NULL || s_confirmation_generation == 0) return;
    if (!s_confirming_format) {
        s_confirming_format = true;
        lv_label_set_text(s_format_label, "Confirm erase");
        lv_label_set_text(s_hint, "This erases all SD card data. Tap again to confirm.");
        return;
    }
    uint32_t generation = s_confirmation_generation;
    s_confirming_format = false;
    s_confirmation_generation = 0;
    lv_label_set_text(s_format_label, "Format FAT32");
    lv_label_set_text(s_hint, "Formatting request sent.");
    szpi_ui_emit(SZPI_UI_EVENT_STORAGE_FORMAT, generation);
}

bool szpi_ui_storage_create(lv_obj_t *panel)
{
    lv_obj_t *status_card = lv_obj_create(panel);
    if (status_card == NULL) return false;
    szpi_ui_style_card(status_card);
    lv_obj_set_size(status_card, 288, 40);
    lv_obj_set_pos(status_card, 0, 0);
    if (storage_label(status_card, "SD CARD", 12, 11, 92, lv_color_hex(0x8B95A5)) == NULL) return false;
    s_status_value = storage_label(status_card, "Checking SD card", 100, 11, 176,
        lv_color_hex(0x34D399));
    if (s_status_value == NULL) return false;

    lv_obj_t *details = lv_obj_create(panel);
    if (details == NULL) return false;
    szpi_ui_style_card(details);
    lv_obj_set_size(details, 288, 78);
    lv_obj_set_pos(details, 0, 46);
    lv_obj_t *capacity_title = storage_label(details, "Capacity", 12, 4, 92,
        lv_color_hex(0x8B95A5));
    s_capacity_value = storage_label(details, "--", 108, 4, 164,
        lv_color_hex(0xE7EAF0));
    lv_obj_t *free_title = storage_label(details, "Available", 12, 22, 92,
        lv_color_hex(0x8B95A5));
    s_free_value = storage_label(details, "--", 108, 22, 164,
        lv_color_hex(0xE7EAF0));
    lv_obj_t *filesystem_title = storage_label(details, "File system", 12, 40, 92,
        lv_color_hex(0x8B95A5));
    s_filesystem_value = storage_label(details, "--", 108, 40, 164,
        lv_color_hex(0xE7EAF0));
    lv_obj_t *speed_title = storage_label(details, "SDMMC", 12, 58, 92,
        lv_color_hex(0x8B95A5));
    s_speed_value = storage_label(details, "--", 108, 58, 164,
        lv_color_hex(0xE7EAF0));
    if (capacity_title == NULL || s_capacity_value == NULL || free_title == NULL ||
        s_free_value == NULL || filesystem_title == NULL || s_filesystem_value == NULL ||
        speed_title == NULL || s_speed_value == NULL) return false;

    s_hint = storage_label(panel, "", 2, 130, 284, lv_color_hex(0xFBBF24));
    if (s_hint != NULL) lv_obj_set_height(s_hint, 40);
    s_retry_button = storage_button(panel, "Retry mount", 0, 124, retry_clicked);
    s_format_button = storage_button(panel, "Format FAT32", 132, 156, format_clicked);
    if (s_hint == NULL || s_retry_button == NULL || s_format_button == NULL) return false;
    s_format_label = lv_obj_get_child(s_format_button, 0);
    s_confirmation_generation = 0;
    s_confirming_format = false;
    s_displayed_state_valid = false;
    s_initialized = true;
    return true;
}

void szpi_ui_storage_update(const szpi_ui_model_t *model)
{
    if (!s_initialized || model == NULL) return;
    if (!s_displayed_state_valid || s_displayed_state != model->storage_state) {
        s_displayed_state = model->storage_state;
        s_displayed_state_valid = true;
        set_label_text(s_status_value, storage_state_text(model->storage_state));
        if (model->storage_state == SZPI_UI_STORAGE_FAULT || model->storage_state == SZPI_UI_STORAGE_NO_CARD) {
            lv_obj_set_style_text_color(s_status_value, lv_color_hex(0xF87171), 0);
        } else if (model->storage_state == SZPI_UI_STORAGE_FORMATTING || model->storage_state == SZPI_UI_STORAGE_BUSY) {
            lv_obj_set_style_text_color(s_status_value, lv_color_hex(0xFBBF24), 0);
        } else {
            lv_obj_set_style_text_color(s_status_value, lv_color_hex(0x34D399), 0);
        }
    }
    char capacity[24] = "--";
    char free_space[24] = "--";
    if (model->storage_capacity_bytes != 0) {
        format_size(model->storage_capacity_bytes, capacity, sizeof(capacity));
    }
    if (model->storage_state == SZPI_UI_STORAGE_READY && model->storage_capacity_bytes != 0) {
        format_size(model->storage_free_bytes, free_space, sizeof(free_space));
    }
    set_label_text(s_capacity_value, capacity);
    set_label_text(s_free_value, free_space);
    set_label_text(s_filesystem_value, filesystem_text(model));
    if (model->storage_max_frequency_khz != 0) {
        char speed[24];
        (void)snprintf(speed, sizeof(speed), "%lu MHz",
            (unsigned long)((model->storage_max_frequency_khz + 500U) / 1000U));
        set_label_text(s_speed_value, speed);
    } else {
        set_label_text(s_speed_value, "--");
    }

    bool busy = model->storage_state == SZPI_UI_STORAGE_BUSY ||
        model->storage_state == SZPI_UI_STORAGE_FORMATTING;
    bool format_available = model->storage_generation != 0 &&
        (model->storage_state == SZPI_UI_STORAGE_READY ||
         model->storage_state == SZPI_UI_STORAGE_CARD_READY_NO_FS);
    if (s_confirming_format && (model->storage_generation != s_confirmation_generation || !format_available)) {
        s_confirming_format = false;
        s_confirmation_generation = 0;
    }
    if (format_available && !s_confirming_format) {
        s_confirmation_generation = model->storage_generation;
    }
    if (!busy && model->storage_state == SZPI_UI_STORAGE_FAULT && model->storage_error_code != 0) {
        char hint[96];
        (void)snprintf(hint, sizeof(hint), "Storage error 0x%lx. Retry or inspect the card.",
            (unsigned long)model->storage_error_code);
        set_label_text(s_hint, hint);
    } else if (s_confirming_format) {
        set_label_text(s_hint, "This erases all SD card data. Tap again to confirm.");
    } else if (model->storage_state == SZPI_UI_STORAGE_FORMATTING) {
        set_label_text(s_hint, "Keep the device powered while formatting.");
    } else if (model->storage_state == SZPI_UI_STORAGE_CARD_READY_NO_FS) {
        set_label_text(s_hint, "No supported filesystem. Format as FAT32 to use this card.");
    } else {
        set_label_text(s_hint, "Formatting erases all data on this SD card.");
    }
    if (busy) {
        if (!lv_obj_has_state(s_retry_button, LV_STATE_DISABLED)) lv_obj_add_state(s_retry_button, LV_STATE_DISABLED);
        if (!lv_obj_has_state(s_format_button, LV_STATE_DISABLED)) lv_obj_add_state(s_format_button, LV_STATE_DISABLED);
        set_label_text(s_format_label, "Formatting...");
    } else {
        if (lv_obj_has_state(s_retry_button, LV_STATE_DISABLED)) lv_obj_remove_state(s_retry_button, LV_STATE_DISABLED);
        if (format_available) {
            if (lv_obj_has_state(s_format_button, LV_STATE_DISABLED)) lv_obj_remove_state(s_format_button, LV_STATE_DISABLED);
            set_label_text(s_format_label, s_confirming_format ? "Confirm erase" : "Format FAT32");
        } else {
            if (!lv_obj_has_state(s_format_button, LV_STATE_DISABLED)) lv_obj_add_state(s_format_button, LV_STATE_DISABLED);
            set_label_text(s_format_label, "Format FAT32");
        }
    }
}

void szpi_ui_storage_destroy(void)
{
    s_status_value = NULL;
    s_capacity_value = NULL;
    s_free_value = NULL;
    s_filesystem_value = NULL;
    s_speed_value = NULL;
    s_hint = NULL;
    s_retry_button = NULL;
    s_format_button = NULL;
    s_format_label = NULL;
    s_confirmation_generation = 0;
    s_confirming_format = false;
    s_initialized = false;
    s_displayed_state_valid = false;
}

void szpi_ui_storage_cancel_confirmation(void)
{
    s_confirming_format = false;
    s_confirmation_generation = 0;
    if (s_format_label != NULL) set_label_text(s_format_label, "Format FAT32");
}
