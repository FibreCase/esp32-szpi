#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "szpi_app.h"
#include "szpi_runtime_internal.h"

#define TAG "szpi_storage_svc"

typedef enum { STORAGE_CMD_RETRY, STORAGE_CMD_FORMAT } storage_command_kind_t;
typedef struct {
    storage_command_kind_t kind;
    uint32_t generation;
} storage_command_t;

static void publish_status(void)
{
    szpi_storage_status_t status = {0};
    esp_err_t err = szpi_storage_get_status(&status);
    if (err != ESP_OK) {
        status.state = SZPI_STORAGE_FAULT;
        status.last_error = err;
    }
    if (szpi_storage_status_lock != NULL && xSemaphoreTake(szpi_storage_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        szpi_storage_status = status;
        xSemaphoreGive(szpi_storage_status_lock);
    }
}

void szpi_storage_service_task(void *context)
{
    (void)context;
    (void)xEventGroupWaitBits(szpi_system_events, SZPI_EVENT_RUNTIME_START, pdFALSE, pdTRUE, portMAX_DELAY);
    esp_err_t err = szpi_storage_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "initial mount unavailable: %s", esp_err_to_name(err));
    publish_status();
    storage_command_t command;
    for (;;) {
        if (xQueueReceive(szpi_storage_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        if (command.kind == STORAGE_CMD_RETRY) {
            (void)szpi_storage_deinit(pdMS_TO_TICKS(1000));
            err = szpi_storage_init();
        } else {
            szpi_audio_service_status_t audio = {0};
            (void)szpi_app_audio_get_status(&audio);
            if (audio.state == SZPI_AUDIO_SERVICE_RECORDING || audio.state == SZPI_AUDIO_SERVICE_PLAYING_TEST ||
                audio.state == SZPI_AUDIO_SERVICE_CAPTURE_TEST || audio.state == SZPI_AUDIO_SERVICE_PLAYING_CAPTURE_TEST ||
                audio.state == SZPI_AUDIO_SERVICE_PLAYING_FILE) {
                (void)szpi_app_audio_stop();
                TickType_t started = xTaskGetTickCount();
                do {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    (void)szpi_app_audio_get_status(&audio);
                } while ((audio.state == SZPI_AUDIO_SERVICE_RECORDING || audio.state == SZPI_AUDIO_SERVICE_PLAYING_TEST ||
                          audio.state == SZPI_AUDIO_SERVICE_CAPTURE_TEST || audio.state == SZPI_AUDIO_SERVICE_PLAYING_CAPTURE_TEST ||
                          audio.state == SZPI_AUDIO_SERVICE_PLAYING_FILE) &&
                         (xTaskGetTickCount() - started) < pdMS_TO_TICKS(5000));
                if (audio.state == SZPI_AUDIO_SERVICE_RECORDING || audio.state == SZPI_AUDIO_SERVICE_PLAYING_TEST ||
                    audio.state == SZPI_AUDIO_SERVICE_CAPTURE_TEST || audio.state == SZPI_AUDIO_SERVICE_PLAYING_CAPTURE_TEST ||
                    audio.state == SZPI_AUDIO_SERVICE_PLAYING_FILE) {
                    publish_status();
                    ESP_LOGE(TAG, "format refused: audio did not stop within the bounded drain window");
                    continue;
                }
            }
            if (szpi_storage_status_lock != NULL && xSemaphoreTake(szpi_storage_status_lock, 0) == pdTRUE) {
                szpi_storage_status.state = SZPI_STORAGE_FORMATTING;
                xSemaphoreGive(szpi_storage_status_lock);
            }
            err = szpi_storage_format_fat32(command.generation, portMAX_DELAY);
        }
        publish_status();
        if (err != ESP_OK) ESP_LOGW(TAG, "storage command=%d failed: %s", (int)command.kind, esp_err_to_name(err));
        else ESP_LOGI(TAG, "storage command=%d complete generation=%" PRIu32, (int)command.kind, command.generation);
    }
}

esp_err_t szpi_app_storage_get_status(szpi_storage_status_t *status)
{
    if (status == NULL || szpi_storage_status_lock == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(szpi_storage_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *status = szpi_storage_status;
    xSemaphoreGive(szpi_storage_status_lock);
    return ESP_OK;
}

static esp_err_t enqueue(storage_command_kind_t kind, uint32_t generation)
{
    if (szpi_storage_queue == NULL) return ESP_ERR_INVALID_STATE;
    storage_command_t command = {.kind = kind, .generation = generation};
    return xQueueSend(szpi_storage_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t szpi_app_storage_format_confirmed(uint32_t generation)
{
    if (generation == 0) return ESP_ERR_INVALID_ARG;
    return enqueue(STORAGE_CMD_FORMAT, generation);
}

esp_err_t szpi_app_storage_retry(void)
{
    return enqueue(STORAGE_CMD_RETRY, 0);
}
