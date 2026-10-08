#include <string.h>
#include "nvs.h"
#include "szpi_wifi.h"
#include "provision_protocol.h"

typedef struct {
    uint32_t version;
    szpi_wifi_config_t config;
} wifi_saved_t;

bool szpi_wifi_hostname_valid(const char *hostname)
{
    if (hostname == NULL) return false;
    size_t length = strnlen(hostname, SZPI_WIFI_HOSTNAME_MAX + 1);
    if (length == 0 || length > SZPI_WIFI_HOSTNAME_MAX || hostname[0] == '-' || hostname[length - 1] == '-') return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)hostname[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

esp_err_t szpi_wifi_load_hostname(char hostname[SZPI_WIFI_HOSTNAME_MAX + 1])
{
    if (hostname == NULL) return ESP_ERR_INVALID_ARG;
    memset(hostname, 0, SZPI_WIFI_HOSTNAME_MAX + 1);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("szpi_wifi", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memcpy(hostname, SZPI_WIFI_HOSTNAME_DEFAULT, sizeof(SZPI_WIFI_HOSTNAME_DEFAULT));
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    size_t size = SZPI_WIFI_HOSTNAME_MAX + 1;
    err = nvs_get_str(nvs, "hostname", hostname, &size);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memcpy(hostname, SZPI_WIFI_HOSTNAME_DEFAULT, sizeof(SZPI_WIFI_HOSTNAME_DEFAULT));
        return ESP_OK;
    }
    if (err == ESP_OK && !szpi_wifi_hostname_valid(hostname)) err = ESP_ERR_INVALID_RESPONSE;
    if (err != ESP_OK) memset(hostname, 0, SZPI_WIFI_HOSTNAME_MAX + 1);
    return err;
}

esp_err_t szpi_wifi_save_hostname(const char *hostname)
{
    if (!szpi_wifi_hostname_valid(hostname)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("szpi_wifi", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, "hostname", hostname);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t szpi_wifi_load_config(szpi_wifi_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    memset(config, 0, sizeof(*config));
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("szpi_wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    wifi_saved_t saved = {0};
    size_t size = sizeof(saved);
    err = nvs_get_blob(nvs, "station", &saved, &size);
    nvs_close(nvs);
    if (err == ESP_OK && (size != sizeof(saved) || saved.version != 1 ||
        !szpi_wifi_credentials_valid(saved.config.ssid, saved.config.password, saved.config.wpa3_only))) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) *config = saved.config;
    memset(&saved, 0, sizeof(saved));
    return err;
}

esp_err_t szpi_wifi_save_config(const szpi_wifi_config_t *config)
{
    if (!config || !szpi_wifi_credentials_valid(config->ssid, config->password, config->wpa3_only)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("szpi_wifi", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    wifi_saved_t saved = {.version = 1, .config = *config};
    err = nvs_set_blob(nvs, "station", &saved, sizeof(saved));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    memset(&saved, 0, sizeof(saved));
    return err;
}

esp_err_t szpi_wifi_forget_config(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("szpi_wifi", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(nvs, "station");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
