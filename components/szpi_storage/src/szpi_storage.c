#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "sdmmc_cmd.h"
#include "freertos/semphr.h"
#include "szpi_board.h"
#include "szpi_storage.h"

#define TAG "szpi_storage"
#define MOUNT_PATH "/sdcard"
#define MAX_OPEN_FILES 4
#define PATH_CAPACITY 160
#define FORMAT_WORK_BYTES 4096

typedef struct {
    FILE *stream;
    char path[PATH_CAPACITY];
} open_file_t;

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static sdmmc_card_t s_card;
static FATFS *s_fs;
static BYTE s_pdrv = FF_DRV_NOT_USED;
static char s_drive[6];
static bool s_host_initialized;
static bool s_disk_registered;
static bool s_vfs_registered;
static bool s_mounted;
static bool s_initialized;
static uint32_t s_generation;
static szpi_storage_status_t s_status = {.state = SZPI_STORAGE_UNINITIALIZED};
static open_file_t s_files[MAX_OPEN_FILES];

static esp_err_t from_fresult(FRESULT result)
{
    switch (result) {
        case FR_OK: return ESP_OK;
        case FR_NO_FILE:
        case FR_NO_PATH: return ESP_ERR_NOT_FOUND;
        case FR_DENIED:
        case FR_EXIST: return ESP_ERR_INVALID_STATE;
        case FR_NOT_READY: return ESP_ERR_INVALID_STATE;
        case FR_NOT_ENOUGH_CORE: return ESP_ERR_NO_MEM;
        default: return ESP_FAIL;
    }
}

static bool path_is_allowed(const char *path)
{
    if (path == NULL || path[0] == '\0' || path[0] == '/' || strlen(path) >= PATH_CAPACITY) return false;
    if (strstr(path, "..") != NULL || strchr(path, ':') != NULL || strchr(path, '\\') != NULL) return false;
    return strncmp(path, "recordings/", sizeof("recordings/") - 1) == 0;
}

static bool has_open_files(void)
{
    for (size_t i = 0; i < MAX_OPEN_FILES; ++i) if (s_files[i].stream != NULL) return true;
    return false;
}

static void update_capacity(void)
{
    s_status.capacity_bytes = (uint64_t)s_card.csd.capacity * s_card.csd.sector_size;
    s_status.max_frequency_khz = s_card.max_freq_khz;
    s_status.free_bytes = 0;
    if (s_mounted) {
        FATFS *fs = s_fs;
        DWORD free_clusters = 0;
        if (f_getfree(s_drive, &free_clusters, &fs) == FR_OK && fs != NULL) {
            s_status.free_bytes = (uint64_t)free_clusters * fs->csize * s_card.csd.sector_size;
        }
        s_status.fat_type = fs != NULL ? fs->fs_type : 0;
    }
}

static esp_err_t init_card_locked(void)
{
    const szpi_board_bindings_t *bindings = NULL;
    ESP_RETURN_ON_ERROR(szpi_board_get_bindings(&bindings), TAG, "get board bindings");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) return err;
    s_host_initialized = true;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = bindings->sd_clk;
    slot.cmd = bindings->sd_cmd;
    slot.d0 = bindings->sd_d0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    if (err != ESP_OK) return err;
    err = sdmmc_card_init(&host, &s_card);
    if (err != ESP_OK) return err;
    err = ff_diskio_get_drive(&s_pdrv);
    if (err != ESP_OK) return err;
    ff_diskio_register_sdmmc(s_pdrv, &s_card);
    s_disk_registered = true;
    (void)snprintf(s_drive, sizeof(s_drive), "%u:", (unsigned)s_pdrv);
    s_status.capacity_bytes = (uint64_t)s_card.csd.capacity * s_card.csd.sector_size;
    s_status.max_frequency_khz = s_card.max_freq_khz;
    s_status.state = SZPI_STORAGE_CARD_READY_NO_FS;
    s_status.last_error = ESP_OK;
    ESP_LOGI(TAG, "SD initialized: capacity=%llu bytes clock=%u kHz width=1",
        (unsigned long long)s_status.capacity_bytes, (unsigned)s_card.max_freq_khz);
    return ESP_OK;
}

static esp_err_t register_vfs_locked(void)
{
    if (s_vfs_registered) return ESP_OK;
    esp_vfs_fat_conf_t conf = {.base_path = MOUNT_PATH, .fat_drive = s_drive, .max_files = MAX_OPEN_FILES};
    esp_err_t err = esp_vfs_fat_register(&conf, &s_fs);
    if (err != ESP_OK) return err;
    s_vfs_registered = true;
    return ESP_OK;
}

static esp_err_t ensure_recordings_dir_locked(void)
{
    char path[sizeof(s_drive) + sizeof("/recordings")];
    int length = snprintf(path, sizeof(path), "%s/recordings", s_drive);
    if (length < 0 || (size_t)length >= sizeof(path)) return ESP_ERR_INVALID_SIZE;
    FRESULT result = f_mkdir(path);
    return result == FR_OK || result == FR_EXIST ? ESP_OK : from_fresult(result);
}

static esp_err_t mount_locked(void)
{
    ESP_RETURN_ON_FALSE(s_disk_registered, ESP_ERR_INVALID_STATE, TAG, "card disk is not registered");
    ESP_RETURN_ON_ERROR(register_vfs_locked(), TAG, "register FAT VFS");
    FRESULT result = f_mount(s_fs, s_drive, 1);
    if (result != FR_OK) {
        (void)f_mount(NULL, s_drive, 0);
        s_status.state = SZPI_STORAGE_CARD_READY_NO_FS;
        s_status.last_error = from_fresult(result);
        return s_status.last_error;
    }
    if (s_fs->fs_type != FS_FAT32) {
        (void)f_mount(NULL, s_drive, 0);
        s_status.state = SZPI_STORAGE_CARD_READY_NO_FS;
        s_status.last_error = ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_mounted = true;
    s_status.state = SZPI_STORAGE_READY;
    s_status.fat_type = s_fs->fs_type;
    s_status.last_error = ESP_OK;
    update_capacity();
    ESP_LOGI(TAG, "mounted FAT32 at %s", MOUNT_PATH);
    esp_err_t dir_err = ensure_recordings_dir_locked();
    if (dir_err != ESP_OK) {
        s_status.state = SZPI_STORAGE_FAULT;
        s_status.last_error = dir_err;
        ESP_LOGW(TAG, "cannot create recordings directory: %s", esp_err_to_name(dir_err));
        return dir_err;
    }
    return ESP_OK;
}

static void cleanup_locked(void)
{
    if (s_mounted) {
        (void)f_mount(NULL, s_drive, 0);
        s_mounted = false;
    }
    if (s_vfs_registered) {
        (void)esp_vfs_fat_unregister_path(MOUNT_PATH);
        s_vfs_registered = false;
        s_fs = NULL;
    }
    if (s_disk_registered) {
        ff_diskio_unregister(s_pdrv);
        s_disk_registered = false;
    }
    if (s_host_initialized) {
        esp_err_t err = sdmmc_host_deinit();
        if (err != ESP_OK) ESP_LOGW(TAG, "SDMMC host cleanup failed: %s", esp_err_to_name(err));
        s_host_initialized = false;
    }
    s_pdrv = FF_DRV_NOT_USED;
    s_drive[0] = '\0';
}

esp_err_t szpi_storage_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    memset(s_files, 0, sizeof(s_files));
    s_status = (szpi_storage_status_t){.state = SZPI_STORAGE_UNINITIALIZED};
    esp_err_t err = init_card_locked();
    if (err == ESP_OK) {
        err = mount_locked();
        if (err == ESP_OK) {
            s_initialized = true;
            s_generation++;
            if (s_generation == 0) s_generation++;
            s_status.generation = s_generation;
        } else {
            // Keep card/disk/VFS alive so an explicit format can recover blank or unsupported media.
            s_initialized = true;
            s_generation++;
            if (s_generation == 0) s_generation++;
            s_status.generation = s_generation;
        }
    } else {
        cleanup_locked();
        s_status.state = SZPI_STORAGE_NO_CARD;
        s_status.last_error = err;
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "storage state=%d err=%s generation=%" PRIu32,
        (int)s_status.state, esp_err_to_name(err), s_status.generation);
    return err == ESP_ERR_NOT_SUPPORTED || s_status.state == SZPI_STORAGE_CARD_READY_NO_FS ? ESP_OK : err;
}

esp_err_t szpi_storage_deinit(TickType_t timeout_ticks)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, timeout_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (has_open_files()) { xSemaphoreGive(s_mutex); return ESP_ERR_INVALID_STATE; }
    cleanup_locked();
    s_initialized = false;
    s_status.state = SZPI_STORAGE_UNINITIALIZED;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t szpi_storage_get_status(szpi_storage_status_t *status)
{
    if (status == NULL || s_mutex == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    update_capacity();
    *status = s_status;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t szpi_storage_format_fat32(uint32_t confirmed_generation, TickType_t timeout_ticks)
{
    if (s_mutex == NULL || !s_initialized) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, timeout_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (confirmed_generation == 0 || confirmed_generation != s_generation || !s_disk_registered || has_open_files() || s_status.state == SZPI_STORAGE_NO_CARD) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = SZPI_STORAGE_FORMATTING;
    uint64_t sector_count = s_status.capacity_bytes / 512U;
    uint64_t cluster_count = s_status.capacity_bytes / (32U * 1024U);
    if (sector_count > UINT32_MAX || cluster_count < 65525U || cluster_count > 0x0FFFFFF5U) {
        s_status.state = s_mounted ? SZPI_STORAGE_READY : SZPI_STORAGE_CARD_READY_NO_FS;
        s_status.last_error = ESP_ERR_NOT_SUPPORTED;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }
    FRESULT fr = f_mount(NULL, s_drive, 0);
    s_mounted = false;
    if (fr != FR_OK) goto format_failed;
    LBA_t partitions[4] = {100, 0, 0, 0};
    uint8_t *work = heap_caps_malloc(FORMAT_WORK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (work == NULL) {
        s_status.state = SZPI_STORAGE_CARD_READY_NO_FS;
        s_status.last_error = ESP_ERR_NO_MEM;
        goto format_done;
    }
    fr = f_fdisk(s_pdrv, partitions, work);
    if (fr == FR_OK) {
        MKFS_PARM options = {.fmt = FM_FAT32, .n_fat = 2, .align = 0, .n_root = 0, .au_size = 32 * 1024};
        fr = f_mkfs(s_drive, &options, work, FORMAT_WORK_BYTES);
    }
    free(work);
    if (fr != FR_OK) goto format_failed;
    fr = f_mount(s_fs, s_drive, 1);
    if (fr != FR_OK || s_fs->fs_type != FS_FAT32) {
        if (fr == FR_OK) fr = FR_NO_FILESYSTEM;
        goto format_failed;
    }
    s_mounted = true;
    s_status.state = SZPI_STORAGE_READY;
    s_status.fat_type = s_fs->fs_type;
    s_status.last_error = ESP_OK;
    update_capacity();
    esp_err_t dir_err = ensure_recordings_dir_locked();
    if (dir_err != ESP_OK) {
        s_status.state = SZPI_STORAGE_FAULT;
        s_status.last_error = dir_err;
        goto format_done;
    }
    // A small independent write/read/delete check is part of the format transaction.
    {
        FILE *probe = fopen(MOUNT_PATH "/.szpi-format-check", "wb+");
        const uint8_t marker[] = {0x53, 0x5A, 0x50, 0x49};
        uint8_t readback[sizeof(marker)] = {0};
        bool ok = probe != NULL && fwrite(marker, 1, sizeof(marker), probe) == sizeof(marker) && fflush(probe) == 0 &&
            fseek(probe, 0, SEEK_SET) == 0 && fread(readback, 1, sizeof(readback), probe) == sizeof(readback) &&
            memcmp(marker, readback, sizeof(marker)) == 0;
        if (probe != NULL) fclose(probe);
        (void)remove(MOUNT_PATH "/.szpi-format-check");
        if (!ok) { s_status.state = SZPI_STORAGE_FAULT; s_status.last_error = ESP_FAIL; }
    }
    goto format_done;
format_failed:
    s_status.state = SZPI_STORAGE_CARD_READY_NO_FS;
    s_status.last_error = fr == FR_OK ? ESP_FAIL : from_fresult(fr);
format_done:
    s_status.generation = ++s_generation;
    if (s_generation == 0) s_status.generation = ++s_generation;
    esp_err_t result = s_status.last_error;
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t szpi_storage_create_recordings_dir(void)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_mounted) { xSemaphoreGive(s_mutex); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = ensure_recordings_dir_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t szpi_storage_open(const char *relative_path, const char *mode, void **file)
{
    if (!path_is_allowed(relative_path) || mode == NULL || file == NULL) return ESP_ERR_INVALID_ARG;
    *file = NULL;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_mounted || s_status.state != SZPI_STORAGE_READY) { xSemaphoreGive(s_mutex); return ESP_ERR_INVALID_STATE; }
    size_t slot = MAX_OPEN_FILES;
    for (size_t i = 0; i < MAX_OPEN_FILES; ++i) if (s_files[i].stream == NULL) { slot = i; break; }
    if (slot == MAX_OPEN_FILES) { xSemaphoreGive(s_mutex); return ESP_ERR_NO_MEM; }
    char full_path[PATH_CAPACITY + sizeof(MOUNT_PATH)];
    (void)snprintf(full_path, sizeof(full_path), MOUNT_PATH "/%s", relative_path);
    FILE *stream = fopen(full_path, mode);
    if (stream == NULL) { xSemaphoreGive(s_mutex); return ESP_FAIL; }
    s_files[slot].stream = stream;
    (void)strlcpy(s_files[slot].path, relative_path, sizeof(s_files[slot].path));
    *file = &s_files[slot];
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

static FILE *valid_file(void *handle)
{
    for (size_t i = 0; i < MAX_OPEN_FILES; ++i) if (handle == &s_files[i]) return s_files[i].stream;
    return NULL;
}

esp_err_t szpi_storage_read(void *file, void *buffer, size_t capacity, size_t *read_bytes)
{
    if (file == NULL || buffer == NULL || read_bytes == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    FILE *stream = valid_file(file);
    if (stream == NULL) { xSemaphoreGive(s_mutex); return ESP_ERR_INVALID_STATE; }
    *read_bytes = fread(buffer, 1, capacity, stream);
    esp_err_t err = ferror(stream) ? ESP_FAIL : ESP_OK;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t szpi_storage_write(void *file, const void *buffer, size_t length, size_t *written_bytes)
{
    if (file == NULL || buffer == NULL || written_bytes == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    FILE *stream = valid_file(file);
    if (stream == NULL) { xSemaphoreGive(s_mutex); return ESP_ERR_INVALID_STATE; }
    *written_bytes = fwrite(buffer, 1, length, stream);
    esp_err_t err = *written_bytes == length ? ESP_OK : ESP_FAIL;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t szpi_storage_seek(void *file, uint64_t offset)
{
    if (file == NULL || offset > LONG_MAX) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    FILE *stream = valid_file(file);
    esp_err_t err = stream == NULL ? ESP_ERR_INVALID_STATE : (fseek(stream, (long)offset, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t szpi_storage_size(void *file, uint64_t *file_bytes)
{
    if (file == NULL || file_bytes == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    FILE *stream = valid_file(file);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (stream != NULL) {
        long current = ftell(stream);
        if (current < 0 || fseek(stream, 0, SEEK_END) != 0) {
            err = ESP_FAIL;
        } else {
            long end = ftell(stream);
            if (end < 0 || fseek(stream, current, SEEK_SET) != 0) err = ESP_FAIL;
            else { *file_bytes = (uint64_t)end; err = ESP_OK; }
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t szpi_storage_sync(void *file)
{
    if (file == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    FILE *stream = valid_file(file);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (stream != NULL) err = fflush(stream) == 0 && fsync(fileno(stream)) == 0 ? ESP_OK : ESP_FAIL;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t szpi_storage_close(void **file)
{
    if (file == NULL || *file == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    FILE *stream = valid_file(*file);
    if (stream == NULL) { xSemaphoreGive(s_mutex); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = fclose(stream) == 0 ? ESP_OK : ESP_FAIL;
    for (size_t i = 0; i < MAX_OPEN_FILES; ++i) if (*file == &s_files[i]) memset(&s_files[i], 0, sizeof(s_files[i]));
    *file = NULL;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t szpi_storage_rename(const char *old_relative_path, const char *new_relative_path)
{
    if (!path_is_allowed(old_relative_path) || !path_is_allowed(new_relative_path)) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_mounted || s_status.state != SZPI_STORAGE_READY) { xSemaphoreGive(s_mutex); return ESP_ERR_INVALID_STATE; }
    char old_path[PATH_CAPACITY + sizeof(MOUNT_PATH)];
    char new_path[PATH_CAPACITY + sizeof(MOUNT_PATH)];
    (void)snprintf(old_path, sizeof(old_path), MOUNT_PATH "/%s", old_relative_path);
    (void)snprintf(new_path, sizeof(new_path), MOUNT_PATH "/%s", new_relative_path);
    struct stat info;
    esp_err_t err = stat(new_path, &info) == 0 ? ESP_ERR_INVALID_STATE : (rename(old_path, new_path) == 0 ? ESP_OK : ESP_FAIL);
    xSemaphoreGive(s_mutex);
    return err;
}
