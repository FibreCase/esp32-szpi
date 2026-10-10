#pragma once

#include "esp_http_server.h"

/* App network-owner task starts the singleton port-80 server. Idempotent for
 * the same callbacks. Handlers run on the HTTP dependency task and must be
 * bounded. Setup callbacks borrow their session state until cleared. */
esp_err_t szpi_http_start(esp_err_t (*get)(httpd_req_t *), esp_err_t (*post)(httpd_req_t *));
/* Network-owner only. Clearing waits for the in-flight setup request; timeout
 * leaves the callbacks intact. No caller may hold service locks here. */
esp_err_t szpi_http_set_setup_handlers(esp_err_t (*get)(httpd_req_t *),
                                     esp_err_t (*post)(httpd_req_t *));
