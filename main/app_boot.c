#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "szpi_app.h"
#include "szpi_board.h"

#define TAG "app_boot"
#define EXPECTED_FLASH_BYTES (16U * 1024U * 1024U)
#define EXPECTED_PSRAM_BYTES (8U * 1024U * 1024U)
#define OTA_SLOT_BYTES 0x7F0000U

static esp_err_t validate_storage(void)
{
    uint32_t flash_size = 0;
    ESP_RETURN_ON_ERROR(esp_flash_get_size(NULL, &flash_size), TAG, "read flash size");
    if (flash_size != EXPECTED_FLASH_BYTES) return ESP_ERR_INVALID_SIZE;

    const esp_partition_t *ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    const esp_partition_t *ota1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1");
    if (ota0 == NULL || ota1 == NULL || ota0->size != OTA_SLOT_BYTES || ota1->size != OTA_SLOT_BYTES ||
        ota0->address + ota0->size > flash_size || ota1->address + ota1->size != flash_size ||
        ota0->address + ota0->size != ota1->address) return ESP_ERR_INVALID_SIZE;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) return ESP_ERR_NOT_FOUND;
    if (running->type != ESP_PARTITION_TYPE_APP ||
        (running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
        (running != ota0 && running != ota1)) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "running partition=%s address=0x%08" PRIx32 " size=0x%08" PRIx32,
        running->label, running->address, running->size);
    ESP_LOGI(TAG, "flash=%" PRIu32 " bytes; ota_0=0x%08" PRIx32 "/0x%08" PRIx32 " ota_1=0x%08" PRIx32 "/0x%08" PRIx32,
        flash_size, ota0->address, ota0->size, ota1->address, ota1->size);
    return ESP_OK;
}

static esp_err_t validate_psram(void)
{
    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM initialized=%s size=%u", esp_psram_is_initialized() ? "yes" : "no", (unsigned)psram_size);
    if (!esp_psram_is_initialized() || psram_size != EXPECTED_PSRAM_BYTES) return ESP_ERR_INVALID_SIZE;
    uint8_t *probe = heap_caps_malloc(256, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (probe == NULL) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < 256; ++i) probe[i] = (uint8_t)(i ^ 0xA5U);
    bool valid = true;
    for (size_t i = 0; i < 256; ++i) if (probe[i] != (uint8_t)(i ^ 0xA5U)) { valid = false; break; }
    heap_caps_free(probe);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void latch_boot_fault(esp_err_t cause)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(TAG, "rejecting unhealthy OTA trial image");
        esp_err_t rollback_err = esp_ota_mark_app_invalid_rollback_and_reboot();
        if (rollback_err == ESP_OK) esp_restart();
        ESP_LOGE(TAG, "OTA rollback request failed: %s", esp_err_to_name(rollback_err));
    }
    ESP_LOGE(TAG, "core startup failed: %s", esp_err_to_name(cause));
    esp_err_t diagnostic_err = szpi_app_runtime_fault(cause);
    if (diagnostic_err != ESP_OK) ESP_LOGE(TAG, "diagnostic runtime unavailable: %s", esp_err_to_name(diagnostic_err));
}

void app_boot_start(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "boot target=%s project=%s version=%s idf=%s cores=%u reset_reason=%d",
        CONFIG_IDF_TARGET, app->project_name, app->version, esp_get_idf_version(), (unsigned)chip.cores, (int)esp_reset_reason());

    esp_err_t err = validate_storage();
    if (err != ESP_OK) { latch_boot_fault(err); return; }
    err = validate_psram();
    if (err != ESP_OK) { latch_boot_fault(err); return; }
    ESP_LOGI(TAG, "heap internal=%" PRIu32 " min=%" PRIu32 " psram=%" PRIu32 " min=%" PRIu32,
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed; data preserved");
        latch_boot_fault(err);
        return;
    }
    err = szpi_board_init();
    if (err != ESP_OK) { latch_boot_fault(err); return; }

    /* Wi-Fi owner loads NVS and starts first setup when no station is saved. */
    err = szpi_app_runtime_start(CONFIG_SZPI_WIFI_ENABLED, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "runtime creation failed: %s", esp_err_to_name(err));
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t state;
        if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_err_t rollback_err = esp_ota_mark_app_invalid_rollback_and_reboot();
            if (rollback_err == ESP_OK) esp_restart();
            ESP_LOGE(TAG, "OTA rollback request failed: %s", esp_err_to_name(rollback_err));
        }
        (void)szpi_board_deinit();
        return;
    }
    err = szpi_app_confirm_boot_health(pdMS_TO_TICKS(30000));
    if (err != ESP_OK) ESP_LOGE(TAG, "application health confirmation failed: %s", esp_err_to_name(err));
}
