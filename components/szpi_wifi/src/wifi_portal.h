#pragma once
#include "szpi_wifi.h"
esp_err_t szpi_portal_start(uint32_t generation, szpi_portal_request_cb_t request_cb,
    szpi_portal_status_cb_t status_cb);
esp_err_t szpi_portal_stop(void);
void szpi_portal_poll(void);
