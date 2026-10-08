#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "szpi_wifi.h"
static unsigned char durable[128], pending[128];
static size_t durable_size, pending_size;
static esp_err_t write_error, commit_error;
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "szpi_wifi"));
    (void)mode; *handle = 1;
    memcpy(pending, durable, durable_size); pending_size = durable_size;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *size)
{
    (void)h; assert(!strcmp(key, "station"));
    if (!durable_size) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= durable_size);
    memcpy(data, durable, durable_size); *size = durable_size;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{
    (void)h; assert(!strcmp(key, "station")); assert(size <= sizeof(pending));
    if (write_error) return write_error;
    memcpy(pending, data, size); pending_size = size;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h; assert(!strcmp(key, "station")); pending_size = 0; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    if (commit_error) return commit_error;
    memcpy(durable, pending, pending_size); durable_size = pending_size;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
int main(void)
{
    szpi_wifi_config_t old = {.ssid = "Old", .password = "old_password"};
    szpi_wifi_config_t next = {.ssid = "New", .password = "new_password"}, loaded;
    assert(szpi_wifi_load_config(&loaded) == ESP_ERR_NVS_NOT_FOUND);
    assert(szpi_wifi_save_config(&old) == ESP_OK);
    write_error = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    assert(szpi_wifi_save_config(&next) != ESP_OK);
    assert(szpi_wifi_load_config(&loaded) == ESP_OK && !strcmp(loaded.ssid, "Old"));
    write_error = ESP_OK; commit_error = ESP_FAIL;
    assert(szpi_wifi_save_config(&next) != ESP_OK);
    assert(szpi_wifi_load_config(&loaded) == ESP_OK && !strcmp(loaded.ssid, "Old"));
    assert(szpi_wifi_forget_config() != ESP_OK);
    assert(szpi_wifi_load_config(&loaded) == ESP_OK);
    commit_error = ESP_OK;
    assert(szpi_wifi_save_config(&next) == ESP_OK);
    assert(szpi_wifi_load_config(&loaded) == ESP_OK && !strcmp(loaded.ssid, "New"));
    durable[0] = 2; /* Unsupported version must never be silently used. */
    assert(szpi_wifi_load_config(&loaded) == ESP_ERR_INVALID_RESPONSE);
    assert(loaded.ssid[0] == 0);
    assert(szpi_wifi_forget_config() == ESP_OK);
    assert(szpi_wifi_load_config(&loaded) == ESP_ERR_NVS_NOT_FOUND);
    puts("Wi-Fi NVS failure handling: PASS");
}
