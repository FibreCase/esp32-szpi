#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "wifi_portal.h"
#include "szpi_http.h"
#include "provision_protocol.h"
#include "portal_address.h"

static int s_dns = -1;
static uint32_t s_generation;
static char s_token[33];
static szpi_portal_request_cb_t s_request;
static szpi_portal_status_cb_t s_status;

static bool socket_ipv4(int fd, bool peer, uint32_t *address)
{
    struct sockaddr_storage storage = {0};
    socklen_t len = sizeof(storage);
    int result = peer ? getpeername(fd, (struct sockaddr *)&storage, &len) :
        getsockname(fd, (struct sockaddr *)&storage, &len);
    if (result) return false;
    if (storage.ss_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        return szpi_portal_address_ipv4(AF_INET,
            &((struct sockaddr_in *)&storage)->sin_addr, AF_INET, AF_INET6, address);
    }
#if CONFIG_LWIP_IPV6
    if (storage.ss_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        return szpi_portal_address_ipv4(AF_INET6,
            &((struct sockaddr_in6 *)&storage)->sin6_addr, AF_INET, AF_INET6, address);
    }
#endif
    return false;
}

static bool ap_client(httpd_req_t *req)
{
    uint32_t local, peer;
    int fd = httpd_req_to_sockfd(req);
    return socket_ipv4(fd, false, &local) && local == 0xC0A80401U &&
        socket_ipv4(fd, true, &peer) && (peer & 0xFFFFFF00U) == 0xC0A80400U &&
        (peer & 255U) > 1 && (peer & 255U) < 255;
}

static bool local_host(httpd_req_t *req)
{
    char host[64];
    return httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK &&
        (!strcmp(host, "192.168.4.1") || !strcmp(host, "192.168.4.1:80"));
}

static esp_err_t error(httpd_req_t *req, const char *status)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "Request rejected", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t json_send(httpd_req_t *req, cJSON *object)
{
    if (!object) return error(req, "503 Service Unavailable");
    char *text = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    if (!text) return error(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return err;
}

static bool authorized(httpd_req_t *req)
{
    char token[sizeof(s_token)] = {0}, origin[64];
    if (!ap_client(req) || !local_host(req) ||
        httpd_req_get_hdr_value_str(req, "X-Setup-Token", token, sizeof(token)) != ESP_OK) return false;
    unsigned diff = 0;
    for (unsigned i = 0; i < sizeof(s_token); i++) diff |= (unsigned char)token[i] ^ (unsigned char)s_token[i];
    if (diff) return false;
    /* A custom header prevents cross-origin HTML forms; no CORS is enabled. */
    if (httpd_req_get_hdr_value_len(req, "Origin")) {
        if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
            (strcmp(origin, "http://192.168.4.1") && strcmp(origin, "http://192.168.4.1:80"))) return false;
    }
    return true;
}

static esp_err_t get_handler(httpd_req_t *req)
{
    if (!ap_client(req)) return error(req, "403 Forbidden");
    if (!local_host(req)) return error(req, "403 Forbidden");
    szpi_provision_status_t status;
    if (!s_status || s_status(&status) != ESP_OK || !status.active || status.generation != s_generation) return error(req, "403 Forbidden");
    cJSON *root = cJSON_CreateObject();
    if (!root) return error(req, "503 Service Unavailable");
    if (!strcmp(req->uri, "/api/setup/session")) {
        cJSON_AddStringToObject(root, "token", s_token);
    } else if (!strcmp(req->uri, "/api/setup/status")) {
        cJSON_AddNumberToObject(root, "state", status.state);
        cJSON_AddStringToObject(root, "message", status.message);
        cJSON_AddBoolToObject(root, "scanning", status.scanning);
        cJSON *list = cJSON_AddArrayToObject(root, "networks");
        if (!list) { cJSON_Delete(root); return error(req, "503 Service Unavailable"); }
        for (unsigned i = 0; i < status.scan_count; i++) {
            cJSON *entry = cJSON_CreateObject();
            if (!entry) { cJSON_Delete(root); return error(req, "503 Service Unavailable"); }
            cJSON_AddStringToObject(entry, "ssid", status.scan[i].ssid);
            cJSON_AddNumberToObject(entry, "rssi", status.scan[i].rssi);
            cJSON_AddBoolToObject(entry, "supported", status.scan[i].supported);
            cJSON_AddBoolToObject(entry, "wpa3_only", status.scan[i].wpa3_only);
            cJSON_AddItemToArray(list, entry);
        }
    } else { cJSON_Delete(root); return error(req, "404 Not Found"); }
    return json_send(req, root);
}

static esp_err_t post_handler(httpd_req_t *req)
{
    if (!authorized(req)) return error(req, "403 Forbidden");
    char body[512] = {0}, type[48];
    if (!req->content_len || req->content_len >= sizeof(body) ||
        httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json")) return error(req, "400 Bad Request");
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + 4000000;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0 || esp_timer_get_time() >= deadline) { memset(body, 0, sizeof(body)); return error(req, "400 Bad Request"); }
        received += n;
    }
    /* Reject escaped NUL, which cJSON would otherwise silently truncate. */
    if (memchr(body, 0, received) || strstr(body, "\\u0000")) { memset(body, 0, sizeof(body)); return error(req, "400 Bad Request"); }
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    memset(body, 0, sizeof(body));
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return error(req, "400 Bad Request"); }
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!strcmp(req->uri, "/api/setup/scan")) {
        err = s_request(SZPI_PORTAL_SCAN, s_generation, NULL);
    } else if (!strcmp(req->uri, "/api/setup/connect")) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
        const cJSON *sae = cJSON_GetObjectItemCaseSensitive(root, "wpa3_only");
        if (cJSON_IsString(ssid) && cJSON_IsString(password) && cJSON_IsBool(sae) &&
            szpi_wifi_credentials_valid(ssid->valuestring, password->valuestring, cJSON_IsTrue(sae))) {
            szpi_wifi_config_t config = {.wpa3_only = cJSON_IsTrue(sae)};
            memcpy(config.ssid, ssid->valuestring, strlen(ssid->valuestring));
            memcpy(config.password, password->valuestring, strlen(password->valuestring));
            err = s_request(SZPI_PORTAL_SUBMIT, s_generation, &config);
            memset(&config, 0, sizeof(config));
        }
        if (cJSON_IsString(password)) memset(password->valuestring, 0, strlen(password->valuestring));
    }
    cJSON_Delete(root);
    if (err != ESP_OK) return error(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "409 Conflict");
    return json_send(req, cJSON_CreateObject());
}

esp_err_t szpi_portal_start(uint32_t generation, szpi_portal_request_cb_t request_cb,
    szpi_portal_status_cb_t status_cb)
{
    if (s_dns >= 0 || !request_cb || !status_cb) return ESP_ERR_INVALID_STATE;
    s_generation = generation;
    s_request = request_cb;
    s_status = status_cb;
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    for (unsigned i = 0; i < 16; i++) snprintf(s_token + i * 2, 3, "%02x", random[i]);
    memset(random, 0, sizeof(random));
    const char *stage = "DNS socket";
    esp_err_t err = ESP_FAIL;
    s_dns = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns < 0) goto fail;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = inet_addr("192.168.4.1")};
    stage = "DNS bind";
    if (bind(s_dns, (struct sockaddr *)&address, sizeof(address))) goto fail;
    stage = "DNS nonblocking";
    if (fcntl(s_dns, F_SETFL, O_NONBLOCK) < 0) goto fail;
    stage = "HTTP setup routes";
    err = szpi_http_set_setup_handlers(get_handler, post_handler);
    if (err != ESP_OK) goto fail;
    return ESP_OK;
fail:
    ESP_LOGE("szpi_portal", "portal start failed at %s: %s errno=%d", stage, esp_err_to_name(err), errno);
    (void)szpi_portal_stop();
    return err;
}

esp_err_t szpi_portal_stop(void)
{
    /* Clearing joins any in-flight handler before releasing session state. */
    esp_err_t err = szpi_http_set_setup_handlers(NULL, NULL);
    if (err != ESP_OK) return err;
    if (s_dns >= 0) { close(s_dns); s_dns = -1; }
    memset(s_token, 0, sizeof(s_token));
    s_request = NULL;
    s_status = NULL;
    return ESP_OK;
}

void szpi_portal_poll(void)
{
    if (s_dns < 0) return;
    for (unsigned i = 0; i < 4; i++) {
        uint8_t query[512], response[528];
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int size = recvfrom(s_dns, query, sizeof(query), 0, (struct sockaddr *)&peer, &len);
        if (size <= 0) break;
        if ((ntohl(peer.sin_addr.s_addr) & 0xFFFFFF00U) != 0xC0A80400U) continue;
        size_t reply = szpi_dns_reply(query, size, response, sizeof(response));
        if (reply) (void)sendto(s_dns, response, reply, 0, (struct sockaddr *)&peer, len);
    }
}
