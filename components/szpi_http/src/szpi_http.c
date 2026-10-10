#include "szpi_http.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static httpd_handle_t s_http;
static bool s_ready;
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static esp_err_t (*s_get)(httpd_req_t *);
static esp_err_t (*s_post)(httpd_req_t *);
static esp_err_t (*s_setup_get)(httpd_req_t *);
static esp_err_t (*s_setup_post)(httpd_req_t *);

static esp_err_t dispatch(httpd_req_t *req)
{
    if (strncmp(req->uri, "/api/setup/", 11) != 0) {
        return (req->method == HTTP_GET ? s_get : s_post)(req);
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "Setup busy", HTTPD_RESP_USE_STRLEN);
    }
    esp_err_t (*handler)(httpd_req_t *) = req->method == HTTP_GET ? s_setup_get : s_setup_post;
    esp_err_t err;
    if (handler != NULL) err = handler(req);
    else {
        httpd_resp_set_status(req, "403 Forbidden");
        err = httpd_resp_send(req, "Setup inactive", HTTPD_RESP_USE_STRLEN);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t szpi_http_start(esp_err_t (*get)(httpd_req_t *), esp_err_t (*post)(httpd_req_t *))
{
    if (get == NULL || post == NULL) return ESP_ERR_INVALID_ARG;
    if (s_http != NULL) return s_ready && s_get == get && s_post == post ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    s_get = get;
    s_post = post;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    esp_err_t err = httpd_start(&s_http, &config);
    if (err != ESP_OK) { s_http = NULL; return err; }
    const httpd_uri_t get_uri = {.uri = "/*", .method = HTTP_GET, .handler = dispatch};
    const httpd_uri_t post_uri = {.uri = "/*", .method = HTTP_POST, .handler = dispatch};
    err = httpd_register_uri_handler(s_http, &get_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_http, &post_uri);
    if (err != ESP_OK) {
        esp_err_t stop_err = httpd_stop(s_http);
        if (stop_err == ESP_OK) s_http = NULL;
        return stop_err == ESP_OK ? err : stop_err;
    }
    s_ready = true;
    return ESP_OK;
}

esp_err_t szpi_http_set_setup_handlers(esp_err_t (*get)(httpd_req_t *),
                                     esp_err_t (*post)(httpd_req_t *))
{
    if (s_http == NULL) return get == NULL && post == NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
    if ((get == NULL) != (post == NULL)) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(4500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_setup_get = get;
    s_setup_post = post;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
