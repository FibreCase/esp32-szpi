#include <string.h>

#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "szpi_runtime_internal.h"

#define TAG "szpi_ota"
#define OTA_HTTP_TIMEOUT_MS 15000
#define OTA_HTTP_BUFFER_BYTES 4096

static void publish_status(szpi_ota_state_t state, uint8_t progress, esp_err_t error)
{
    if (szpi_ota_status_lock == NULL || xSemaphoreTake(szpi_ota_status_lock, portMAX_DELAY) != pdTRUE) return;
    szpi_ota_status.state = state;
    szpi_ota_status.progress_percent = progress;
    szpi_ota_status.last_error = error;
    xSemaphoreGive(szpi_ota_status_lock);
}

static esp_err_t perform_ota(void)
{
    if ((xEventGroupGetBits(szpi_system_events) & SZPI_EVENT_NETWORK_READY) == 0) return ESP_ERR_INVALID_STATE;
    if (CONFIG_SZPI_OTA_URL[0] == '\0') return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t http_config = {
        .url = CONFIG_SZPI_OTA_URL,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = OTA_HTTP_BUFFER_BYTES,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota_config = {.http_config = &http_config};
    esp_https_ota_handle_t handle = NULL;
    publish_status(SZPI_OTA_CONNECTING, 0, ESP_OK);
    ESP_LOGI(TAG, "starting update from configured URL: %s", CONFIG_SZPI_OTA_URL);

    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) return err;

    const esp_app_desc_t *running_app = esp_app_get_description();
    esp_app_desc_t update_app = {0};
    err = esp_https_ota_get_img_desc(handle, &update_app);
    if (err == ESP_OK && (running_app == NULL ||
        strncmp(update_app.project_name, running_app->project_name, sizeof(update_app.project_name)) != 0)) {
        err = ESP_ERR_INVALID_RESPONSE;
        ESP_LOGE(TAG, "downloaded image belongs to a different project");
    }
    if (err != ESP_OK) {
        (void)esp_https_ota_abort(handle);
        return err;
    }
    ESP_LOGI(TAG, "image version=%s project=%s", update_app.version, update_app.project_name);

    publish_status(SZPI_OTA_DOWNLOADING, 0, ESP_OK);
    do {
        err = esp_https_ota_perform(handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int image_size = esp_https_ota_get_image_size(handle);
            int bytes_read = esp_https_ota_get_image_len_read(handle);
            uint8_t progress = 0;
            if (image_size > 0 && bytes_read > 0) {
                uint32_t calculated = (uint32_t)bytes_read * 100U / (uint32_t)image_size;
                progress = (uint8_t)(calculated > 99U ? 99U : calculated);
            }
            publish_status(SZPI_OTA_DOWNLOADING, progress, ESP_OK);
            vTaskDelay(1);
        }
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(handle)) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) {
        err = esp_https_ota_finish(handle);
        handle = NULL;
    } else {
        esp_err_t abort_err = esp_https_ota_abort(handle);
        handle = NULL;
        if (abort_err != ESP_OK) ESP_LOGW(TAG, "aborting partial image failed: %s", esp_err_to_name(abort_err));
    }
    if (handle != NULL) (void)esp_https_ota_abort(handle);
    if (err == ESP_OK) ESP_LOGI(TAG, "image validated and boot partition selected");
    return err;
}

void szpi_ota_service_task(void *context)
{
    (void)context;
    (void)xEventGroupWaitBits(szpi_system_events, SZPI_EVENT_RUNTIME_START, pdFALSE, pdTRUE, portMAX_DELAY);
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        esp_err_t err = perform_ota();
        if (err != ESP_OK) {
            publish_status(SZPI_OTA_FAILED, 0, err);
            ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
            continue;
        }
        publish_status(SZPI_OTA_RESTARTING, 100, ESP_OK);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}
