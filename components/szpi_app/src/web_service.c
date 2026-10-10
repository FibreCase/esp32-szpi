#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "szpi_app.h"
#include "szpi_http.h"
#include "szpi_runtime_internal.h"

#define TAG "szpi_web"
extern const uint8_t dashboard_gz_start[] asm("_binary_dashboard_html_gz_start");
extern const uint8_t dashboard_gz_end[] asm("_binary_dashboard_html_gz_end");
static char s_control_token[33];
static bool s_started;

static esp_err_t reply_error(httpd_req_t *req, const char *status)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "Request rejected", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t reply_json(httpd_req_t *req, cJSON *root)
{
    if (root == NULL) return reply_error(req, "503 Service Unavailable");
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) return reply_error(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return err;
}

static bool local_address(httpd_req_t *req, char *text, size_t size, bool *is_ap)
{
    struct sockaddr_storage address = {0};
    socklen_t length = sizeof(address);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&address, &length) != 0) return false;
    struct in_addr ipv4;
    if (address.ss_family == AF_INET) ipv4 = ((struct sockaddr_in *)&address)->sin_addr;
    else if (address.ss_family == AF_INET6 &&
             IN6_IS_ADDR_V4MAPPED(&((struct sockaddr_in6 *)&address)->sin6_addr)) {
        memcpy(&ipv4, &((struct sockaddr_in6 *)&address)->sin6_addr.s6_addr[12], sizeof(ipv4));
    } else return false;
    *is_ap = ntohl(ipv4.s_addr) == 0xC0A80401U;
    return inet_ntop(AF_INET, &ipv4, text, size) != NULL;
}

static bool valid_host(httpd_req_t *req)
{
    char host[80], ip[16], expected[80];
    bool ap;
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        !local_address(req, ip, sizeof(ip), &ap)) return false;
    if (!strcmp(host, ip)) return true;
    snprintf(expected, sizeof(expected), "%s:80", ip);
    if (!strcmp(host, expected)) return true;
    szpi_wifi_status_t wifi = {0};
    if (szpi_app_wifi_get_status(&wifi) != ESP_OK || wifi.hostname[0] == '\0') return false;
    snprintf(expected, sizeof(expected), "%s.local", wifi.hostname);
    if (!strcmp(host, expected)) return true;
    snprintf(expected, sizeof(expected), "%s.local:80", wifi.hostname);
    return !strcmp(host, expected);
}

static bool authorized(httpd_req_t *req)
{
    char token[33] = {0}, host[80], origin[88], expected[88];
    if (!valid_host(req) ||
        httpd_req_get_hdr_value_str(req, "X-Control-Token", token, sizeof(token)) != ESP_OK) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < sizeof(token); ++i) diff |= (unsigned char)token[i] ^ (unsigned char)s_control_token[i];
    if (diff != 0) return false;
    if (httpd_req_get_hdr_value_len(req, "Origin") != 0) {
        if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
            httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) return false;
        snprintf(expected, sizeof(expected), "http://%s", host);
        if (strcmp(origin, expected) != 0) return false;
    }
    return true;
}

static esp_err_t device_status(httpd_req_t *req)
{
    szpi_wifi_status_t wifi = {0};
    szpi_ui_status_t ui = {0};
    szpi_audio_service_status_t audio = {0};
    szpi_camera_preview_status_t camera = {0};
    szpi_storage_status_t storage = {0};
    bool wifi_valid = szpi_app_wifi_get_status(&wifi) == ESP_OK;
    bool ui_valid = szpi_app_ui_get_status(&ui) == ESP_OK &&
        (ui.state == SZPI_UI_READY || ui.state == SZPI_UI_TOUCH_FAULT);
    bool audio_valid = szpi_app_audio_get_status(&audio) == ESP_OK &&
        szpi_runtime_audio_available();
    bool camera_valid = szpi_app_camera_preview_get_status(&camera) == ESP_OK;
    bool storage_valid = szpi_app_storage_get_status(&storage) == ESP_OK;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return reply_error(req, "503 Service Unavailable");
    cJSON *network = cJSON_AddObjectToObject(root, "network");
    cJSON *settings = cJSON_AddObjectToObject(root, "settings");
    cJSON *camera_json = cJSON_AddObjectToObject(root, "camera");
    cJSON *storage_json = cJSON_AddObjectToObject(root, "storage");
    if (network == NULL || settings == NULL || camera_json == NULL || storage_json == NULL) {
        cJSON_Delete(root);
        return reply_error(req, "503 Service Unavailable");
    }
    const esp_partition_t *partition = esp_ota_get_running_partition();
    cJSON_AddStringToObject(root, "hostname", wifi_valid ? wifi.hostname : "");
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "slot", partition ? partition->label : "Unavailable");
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    bool online = wifi_valid && wifi.state == SZPI_WIFI_ONLINE;
    cJSON_AddBoolToObject(network, "online", online);
    cJSON_AddStringToObject(network, "ssid", online ? wifi.ssid : "");
    char ip[16] = "";
    if (online) snprintf(ip, sizeof(ip), IPSTR, IP2STR(&wifi.ip_info.ip));
    cJSON_AddStringToObject(network, "ip", ip);
    if (online && wifi.rssi_valid) cJSON_AddNumberToObject(network, "rssi", wifi.rssi);
    else cJSON_AddNullToObject(network, "rssi");
    char saved_hostname[SZPI_WIFI_HOSTNAME_MAX + 1];
    if (szpi_wifi_load_hostname(saved_hostname) == ESP_OK) {
        cJSON_AddStringToObject(settings, "hostname", saved_hostname);
    } else {
        cJSON_AddNullToObject(settings, "hostname");
    }
    cJSON_AddBoolToObject(settings, "display_available", ui_valid);
    cJSON_AddBoolToObject(settings, "audio_available", audio_valid);
    if (ui_valid) cJSON_AddNumberToObject(settings, "brightness", ui.brightness_percent);
    else cJSON_AddNullToObject(settings, "brightness");
    if (audio_valid) {
        cJSON_AddNumberToObject(settings, "speaker", audio.output_volume_percent);
        cJSON_AddNumberToObject(settings, "microphone", audio.input_gain_percent);
    } else {
        cJSON_AddNullToObject(settings, "speaker");
        cJSON_AddNullToObject(settings, "microphone");
    }
    if (camera_valid) {
        cJSON_AddNumberToObject(camera_json, "state", camera.state);
        cJSON_AddNumberToObject(camera_json, "fps_milli", camera.display_fps_milli);
    } else {
        cJSON_AddNullToObject(camera_json, "state");
        cJSON_AddNumberToObject(camera_json, "fps_milli", 0);
    }
    if (storage_valid) {
        cJSON_AddNumberToObject(storage_json, "state", storage.state);
        cJSON_AddNumberToObject(storage_json, "capacity_bytes", (double)storage.capacity_bytes);
        cJSON_AddNumberToObject(storage_json, "free_bytes", (double)storage.free_bytes);
    } else {
        cJSON_AddNullToObject(storage_json, "state");
        cJSON_AddNullToObject(storage_json, "capacity_bytes");
        cJSON_AddNullToObject(storage_json, "free_bytes");
    }
    if (cJSON_GetArraySize(root) != 10 || cJSON_GetArraySize(network) != 4 ||
        cJSON_GetArraySize(settings) != 6 || cJSON_GetArraySize(camera_json) != 2 ||
        cJSON_GetArraySize(storage_json) != 3) {
        cJSON_Delete(root);
        return reply_error(req, "503 Service Unavailable");
    }
    return reply_json(req, root);
}

static esp_err_t get_handler(httpd_req_t *req)
{
    bool ap = false;
    char ip[16];
    (void)local_address(req, ip, sizeof(ip), &ap);
    bool page = !strcmp(req->uri, "/") || !strcmp(req->uri, "/settings") || !strcmp(req->uri, "/setup");
    if (ap && (strcmp(req->uri, "/setup") != 0) &&
        (!strcmp(req->uri, "/") || (!page && strncmp(req->uri, "/api/", 5) != 0))) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
        return httpd_resp_send(req, "Open Wi-Fi setup", HTTPD_RESP_USE_STRLEN);
    }
    if (!valid_host(req)) return reply_error(req, "403 Forbidden");
    if (page) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
        httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'");
        return httpd_resp_send(req, (const char *)dashboard_gz_start, dashboard_gz_end - dashboard_gz_start);
    }
    if (!strcmp(req->uri, "/api/device")) return device_status(req);
    if (!strcmp(req->uri, "/api/control-session")) {
        cJSON *root = cJSON_CreateObject();
        if (root != NULL && cJSON_AddStringToObject(root, "token", s_control_token) == NULL) {
            cJSON_Delete(root);
            root = NULL;
        }
        return reply_json(req, root);
    }
    return reply_error(req, "404 Not Found");
}

static esp_err_t post_handler(httpd_req_t *req)
{
    if (strcmp(req->uri, "/api/settings") != 0) return reply_error(req, "404 Not Found");
    if (!authorized(req)) return reply_error(req, "403 Forbidden");
    char body[128] = {0}, type[48];
    if (req->content_len == 0 || req->content_len >= sizeof(body) ||
        httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") != 0) return reply_error(req, "400 Bad Request");
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + 4000000;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0 || esp_timer_get_time() >= deadline) return reply_error(req, "400 Bad Request");
        received += n;
    }
    if (memchr(body, 0, received) || strstr(body, "\\u0000")) return reply_error(req, "400 Bad Request");
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsObject(root) && cJSON_GetArraySize(root) == 2 && cJSON_IsString(key) &&
        !strcmp(key->valuestring, "hostname") && cJSON_IsString(value)) {
        // Persist only: DHCP, mDNS and the active status keep the boot hostname.
        err = szpi_wifi_save_hostname(value->valuestring);
    } else if (cJSON_IsObject(root) && cJSON_GetArraySize(root) == 2 && cJSON_IsString(key) &&
        cJSON_IsNumber(value) && isfinite(value->valuedouble) && value->valuedouble >= 0 &&
        value->valuedouble <= 100 && value->valuedouble == (double)value->valueint) {
        if (!strcmp(key->valuestring, "brightness") && value->valueint >= 10) {
            err = szpi_app_ui_set_brightness((uint8_t)value->valueint);
        } else if (!strcmp(key->valuestring, "speaker")) {
            err = szpi_runtime_audio_available() ? szpi_app_audio_set_volume((uint8_t)value->valueint) : ESP_ERR_INVALID_STATE;
            if (err == ESP_OK) err = szpi_app_audio_save_settings();
        } else if (!strcmp(key->valuestring, "microphone")) {
            err = szpi_runtime_audio_available() ? szpi_app_audio_set_input_gain_percent((uint8_t)value->valueint) : ESP_ERR_INVALID_STATE;
            if (err == ESP_OK) err = szpi_app_audio_save_settings();
        }
    }
    cJSON_Delete(root);
    if (err != ESP_OK) return reply_error(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "409 Conflict");
    httpd_resp_set_status(req, "202 Accepted");
    return reply_json(req, cJSON_CreateObject());
}

esp_err_t szpi_web_service_start(void)
{
    if (s_started) return ESP_OK;
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) snprintf(s_control_token + i * 2, 3, "%02x", random[i]);
    esp_err_t err = szpi_http_start(get_handler, post_handler);
    if (err == ESP_OK) {
        s_started = true;
        ESP_LOGI(TAG, "control panel listening on port 80");
    }
    return err;
}
