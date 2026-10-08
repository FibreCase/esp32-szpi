#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "szpi_app.h"
#include "szpi_input.h"

void szpi_ui_pages_create(void);
void szpi_ui_pages_destroy(void);
void szpi_ui_pages_update_status(uint32_t runtime_seconds, bool touch_pressed,
                                 uint16_t touch_x, uint16_t touch_y, bool touch_faulted);
void szpi_ui_pages_update_inputs(bool imu_ready, const szpi_imu_info_t *imu_info,
                                 const szpi_imu_sample_t *imu_sample, bool boot_ready,
                                 const szpi_boot_state_t *boot_state,
                                 uint32_t boot_short_count, uint32_t boot_long_count);
bool szpi_ui_pages_camera_active(void);
void szpi_ui_pages_set_camera_source(uint8_t *data);
void szpi_ui_pages_clear_camera_source(void);
void szpi_ui_pages_show_camera_frame(void);
void szpi_ui_pages_show_home(void);
uint8_t szpi_ui_pages_brightness(void);
void szpi_ui_service_retry_inputs(void);
