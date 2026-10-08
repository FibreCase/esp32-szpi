#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    SZPI_STORAGE_UNINITIALIZED,
    SZPI_STORAGE_NO_CARD,
    SZPI_STORAGE_CARD_READY_NO_FS,
    SZPI_STORAGE_READY,
    SZPI_STORAGE_BUSY,
    SZPI_STORAGE_FORMATTING,
    SZPI_STORAGE_FAULT,
} szpi_storage_state_t;

typedef struct {
    szpi_storage_state_t state;
    uint64_t capacity_bytes;
    uint64_t free_bytes;
    uint32_t generation;
    uint32_t max_frequency_khz;
    uint8_t fat_type;
    esp_err_t last_error;
} szpi_storage_status_t;

// Task-context APIs. Calls serialize internally and time out while another operation owns the card.
esp_err_t szpi_storage_init(void);
esp_err_t szpi_storage_deinit(TickType_t timeout_ticks);
esp_err_t szpi_storage_get_status(szpi_storage_status_t *status);
// Destructive: caller must present the current mount generation after its explicit UI confirmation.
esp_err_t szpi_storage_format_fat32(uint32_t confirmed_generation, TickType_t timeout_ticks);
esp_err_t szpi_storage_create_recordings_dir(void);
esp_err_t szpi_storage_open(const char *relative_path, const char *mode, void **file);
esp_err_t szpi_storage_read(void *file, void *buffer, size_t capacity, size_t *read_bytes);
esp_err_t szpi_storage_write(void *file, const void *buffer, size_t length, size_t *written_bytes);
esp_err_t szpi_storage_seek(void *file, uint64_t offset);
esp_err_t szpi_storage_size(void *file, uint64_t *file_bytes);
// Flush stdio buffers and force the VFS/FatFs descriptor to media.
esp_err_t szpi_storage_sync(void *file);
esp_err_t szpi_storage_close(void **file);
esp_err_t szpi_storage_rename(const char *old_relative_path, const char *new_relative_path);
