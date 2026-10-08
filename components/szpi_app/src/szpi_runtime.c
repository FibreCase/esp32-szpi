#include <inttypes.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "szpi_board.h"
#include "szpi_display.h"
#include "szpi_input.h"
#include "szpi_audio.h"
#include "szpi_runtime_internal.h"

#define TAG "szpi_runtime"
#define QUEUE_DEPTH 16
#define SUPERVISOR_STACK_BYTES 4096
#define WIFI_STACK_BYTES 4096
#define UI_STACK_BYTES 6144
#define PREVIEW_STACK_BYTES 4096
#define STORAGE_STACK_BYTES 4096
#define STORAGE_QUEUE_DEPTH 8
#define AUDIO_QUEUE_DEPTH 8
#define AUDIO_STACK_BYTES 6144
#define PREVIEW_QUEUE_DEPTH 1
#define PREVIEW_CMD_START (1U << 0)
#define PREVIEW_CMD_STOP (1U << 1)

static StaticQueue_t s_wifi_queue_storage;
static uint8_t s_wifi_queue_buffer[QUEUE_DEPTH * sizeof(wifi_message_t)];
static StaticQueue_t s_supervisor_queue_storage;
static uint8_t s_supervisor_queue_buffer[QUEUE_DEPTH * sizeof(uint32_t)];
static StaticEventGroup_t s_events_storage;
static StaticSemaphore_t s_status_mutex_storage;
static StaticSemaphore_t s_ui_status_mutex_storage;
static StaticSemaphore_t s_preview_status_mutex_storage;
static StaticTask_t s_supervisor_tcb;
static StaticTask_t s_wifi_tcb;
static StaticTask_t s_ui_tcb;
static StaticTask_t s_preview_tcb;
static StaticTask_t s_storage_tcb;
static StaticTask_t s_audio_tcb;
static StackType_t s_supervisor_stack[SUPERVISOR_STACK_BYTES / sizeof(StackType_t)];
static StackType_t s_wifi_stack[WIFI_STACK_BYTES / sizeof(StackType_t)];
static StackType_t s_ui_stack[UI_STACK_BYTES / sizeof(StackType_t)];
static StackType_t s_preview_stack[PREVIEW_STACK_BYTES / sizeof(StackType_t)];
static StackType_t s_storage_stack[STORAGE_STACK_BYTES / sizeof(StackType_t)];
static StackType_t s_audio_stack[AUDIO_STACK_BYTES / sizeof(StackType_t)];
static StaticQueue_t s_preview_frame_queue_storage;
static uint8_t s_preview_frame_queue_buffer[PREVIEW_QUEUE_DEPTH * sizeof(szpi_preview_frame_message_t)];
static StaticQueue_t s_preview_ack_queue_storage;
static uint8_t s_preview_ack_queue_buffer[PREVIEW_QUEUE_DEPTH * sizeof(szpi_preview_ack_message_t)];
static StaticQueue_t s_storage_queue_storage;
static uint8_t s_storage_queue_buffer[STORAGE_QUEUE_DEPTH * 8U];
static StaticSemaphore_t s_storage_status_mutex_storage;
static StaticQueue_t s_audio_queue_storage;
static uint8_t s_audio_queue_buffer[AUDIO_QUEUE_DEPTH * sizeof(uint32_t)];
static StaticSemaphore_t s_audio_status_mutex_storage;
QueueHandle_t szpi_wifi_queue;
QueueHandle_t szpi_supervisor_queue;
EventGroupHandle_t szpi_system_events;
SemaphoreHandle_t szpi_wifi_status_lock;
szpi_wifi_status_t szpi_wifi_status;
SemaphoreHandle_t szpi_ui_status_lock;
szpi_ui_status_t szpi_ui_status;
QueueHandle_t szpi_preview_frame_queue;
QueueHandle_t szpi_preview_ack_queue;
SemaphoreHandle_t szpi_preview_status_lock;
szpi_camera_preview_status_t szpi_preview_status;
TaskHandle_t szpi_preview_task_handle;
QueueHandle_t szpi_storage_queue;
SemaphoreHandle_t szpi_storage_status_lock;
szpi_storage_status_t szpi_storage_status;
QueueHandle_t szpi_audio_queue;
SemaphoreHandle_t szpi_audio_status_lock;
szpi_audio_service_status_t szpi_audio_status;
static bool s_runtime_started;
static TaskHandle_t s_supervisor_task_handle;
static TaskHandle_t s_wifi_task_handle;
static TaskHandle_t s_ui_task_handle;
static TaskHandle_t s_storage_task_handle;
static TaskHandle_t s_audio_task_handle;
static bool s_ui_enabled;
static portMUX_TYPE s_peak_mux = portMUX_INITIALIZER_UNLOCKED;
static UBaseType_t s_wifi_queue_peak;
static UBaseType_t s_supervisor_queue_peak;

typedef struct {
    const char *name;
    TaskFunction_t entry;
    uint32_t stack_bytes;
    UBaseType_t priority;
    BaseType_t core_id;
    bool enabled;
    StackType_t *stack;
    StaticTask_t *tcb;
} runtime_task_descriptor_t;

typedef struct {
    const char *name;
    const char *owner;
    uint32_t capacity;
} runtime_resource_descriptor_t;

static const runtime_resource_descriptor_t s_resource_table[] = {
    {"supervisor_queue", "szpi_app", QUEUE_DEPTH},
    {"wifi_queue", "szpi_app", QUEUE_DEPTH},
    {"system_events", "szpi_app", 0},
    {"wifi_status_mutex", "szpi_app", 1},
    {"ui_status_mutex", "szpi_app", 1},
    {"ui_draw_buffers", "szpi_display", 2U * SZPI_DISPLAY_BUFFER_BYTES},
    {"preview_frame_queue", "szpi_app", PREVIEW_QUEUE_DEPTH},
    {"preview_ack_queue", "szpi_app", PREVIEW_QUEUE_DEPTH},
    {"camera_framebuffer", "szpi_camera", SZPI_CAMERA_FRAME_BYTES},
    {"preview_staging", "szpi_app_ui", SZPI_CAMERA_FRAME_BYTES},
    {"imu_i2c_device", "szpi_input (borrowed board I2C)", 1},
    {"imu_sample_state", "szpi_input", sizeof(szpi_imu_sample_t)},
    {"boot_gpio_state", "szpi_input", sizeof(szpi_boot_state_t)},
    {"storage_command_queue", "szpi_app storage service", STORAGE_QUEUE_DEPTH},
    {"storage_status_mutex", "szpi_app", 1},
    {"audio_command_queue", "szpi_app audio service", AUDIO_QUEUE_DEPTH},
    {"audio_status_mutex", "szpi_app", 1},
};

static esp_err_t create_runtime_resources(szpi_wifi_service_state_t initial_wifi_state)
{
    szpi_wifi_queue = xQueueCreateStatic(QUEUE_DEPTH, sizeof(wifi_message_t), s_wifi_queue_buffer, &s_wifi_queue_storage);
    szpi_supervisor_queue = xQueueCreateStatic(QUEUE_DEPTH, sizeof(uint32_t), s_supervisor_queue_buffer, &s_supervisor_queue_storage);
    szpi_system_events = xEventGroupCreateStatic(&s_events_storage);
    szpi_wifi_status_lock = xSemaphoreCreateMutexStatic(&s_status_mutex_storage);
    szpi_ui_status_lock = xSemaphoreCreateMutexStatic(&s_ui_status_mutex_storage);
    szpi_preview_frame_queue = xQueueCreateStatic(PREVIEW_QUEUE_DEPTH, sizeof(szpi_preview_frame_message_t),
        s_preview_frame_queue_buffer, &s_preview_frame_queue_storage);
    szpi_preview_ack_queue = xQueueCreateStatic(PREVIEW_QUEUE_DEPTH, sizeof(szpi_preview_ack_message_t),
        s_preview_ack_queue_buffer, &s_preview_ack_queue_storage);
    szpi_preview_status_lock = xSemaphoreCreateMutexStatic(&s_preview_status_mutex_storage);
    szpi_storage_queue = xQueueCreateStatic(STORAGE_QUEUE_DEPTH, 8U, s_storage_queue_buffer, &s_storage_queue_storage);
    szpi_storage_status_lock = xSemaphoreCreateMutexStatic(&s_storage_status_mutex_storage);
    szpi_audio_queue = xQueueCreateStatic(AUDIO_QUEUE_DEPTH, sizeof(uint32_t), s_audio_queue_buffer, &s_audio_queue_storage);
    szpi_audio_status_lock = xSemaphoreCreateMutexStatic(&s_audio_status_mutex_storage);
    if (szpi_wifi_queue == NULL || szpi_supervisor_queue == NULL || szpi_system_events == NULL ||
        szpi_wifi_status_lock == NULL || szpi_ui_status_lock == NULL || szpi_preview_frame_queue == NULL ||
        szpi_preview_ack_queue == NULL || szpi_preview_status_lock == NULL || szpi_storage_queue == NULL ||
        szpi_storage_status_lock == NULL) return ESP_ERR_NO_MEM;
    if (szpi_audio_queue == NULL || szpi_audio_status_lock == NULL) return ESP_ERR_NO_MEM;
    szpi_wifi_status = (szpi_wifi_status_t){.state = initial_wifi_state};
    szpi_ui_status = (szpi_ui_status_t){.state = SZPI_UI_STOPPED};
    szpi_preview_status = (szpi_camera_preview_status_t){.state = SZPI_CAMERA_PREVIEW_STOPPED};
    szpi_storage_status = (szpi_storage_status_t){.state = SZPI_STORAGE_UNINITIALIZED};
    szpi_audio_status = (szpi_audio_service_status_t){
        .state = SZPI_AUDIO_SERVICE_OFFLINE,
        .input_gain_db = SZPI_AUDIO_DEFAULT_INPUT_GAIN_DB,
        .output_volume_percent = SZPI_AUDIO_DEFAULT_VOLUME_PERCENT,
    };
    return ESP_OK;
}

void szpi_runtime_record_queue_peaks(void)
{
    UBaseType_t wifi_waiting = uxQueueMessagesWaiting(szpi_wifi_queue);
    UBaseType_t supervisor_waiting = uxQueueMessagesWaiting(szpi_supervisor_queue);
    portENTER_CRITICAL(&s_peak_mux);
    if (wifi_waiting > s_wifi_queue_peak) s_wifi_queue_peak = wifi_waiting;
    if (supervisor_waiting > s_supervisor_queue_peak) s_supervisor_queue_peak = supervisor_waiting;
    portEXIT_CRITICAL(&s_peak_mux);
}

static esp_err_t create_tasks(bool wifi_enabled, bool have_config, bool ui_enabled)
{
    runtime_task_descriptor_t task_table[] = {
        {"szpi_supervisor", szpi_supervisor_task, SUPERVISOR_STACK_BYTES, 3, tskNO_AFFINITY, true, s_supervisor_stack, &s_supervisor_tcb},
        {"szpi_wifi", szpi_wifi_service_task, WIFI_STACK_BYTES, 4, tskNO_AFFINITY, wifi_enabled && have_config, s_wifi_stack, &s_wifi_tcb},
        {"szpi_ui", szpi_ui_service_task, UI_STACK_BYTES, 5, tskNO_AFFINITY, ui_enabled, s_ui_stack, &s_ui_tcb},
        {"szpi_preview", szpi_camera_preview_service_task, PREVIEW_STACK_BYTES, 4, tskNO_AFFINITY, ui_enabled, s_preview_stack, &s_preview_tcb},
        {"szpi_storage", szpi_storage_service_task, STORAGE_STACK_BYTES, 6, tskNO_AFFINITY, ui_enabled, s_storage_stack, &s_storage_tcb},
        {"szpi_audio_rx", szpi_audio_service_task, AUDIO_STACK_BYTES, 8, tskNO_AFFINITY, ui_enabled, s_audio_stack, &s_audio_tcb},
    };
    for (size_t i = 0; i < sizeof(task_table) / sizeof(task_table[0]); ++i) {
        runtime_task_descriptor_t *task = &task_table[i];
        if (!task->enabled) continue;
        TaskHandle_t handle = xTaskCreateStaticPinnedToCore(task->entry, task->name,
            task->stack_bytes / sizeof(StackType_t), NULL, task->priority,
            task->stack, task->tcb, task->core_id);
        if (handle == NULL) {
            if (i == 2 || i == 3 || i == 4 || i == 5) {
                if (i == 2) {
                    szpi_ui_status.state = SZPI_UI_DISPLAY_FAULT;
                    szpi_ui_status.last_error = ESP_ERR_NO_MEM;
                    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_DISPLAY_FAULT | SZPI_EVENT_UI_STOPPED);
                } else if (i == 3) {
                    szpi_preview_status.state = SZPI_CAMERA_PREVIEW_UNAVAILABLE;
                    szpi_preview_status.last_error = ESP_ERR_NO_MEM;
                    szpi_preview_status.error_count++;
                } else if (i == 4) {
                    szpi_storage_status.state = SZPI_STORAGE_FAULT;
                    szpi_storage_status.last_error = ESP_ERR_NO_MEM;
                } else {
                    szpi_audio_status.state = SZPI_AUDIO_SERVICE_FAULT;
                    szpi_audio_status.last_error = ESP_ERR_NO_MEM;
                    szpi_audio_status.errors++;
                }
                ESP_LOGE(TAG, "%s task creation failed; continuing with available services", task->name);
                continue;
            }
            if (s_supervisor_task_handle != NULL) {
                vTaskDelete(s_supervisor_task_handle);
                s_supervisor_task_handle = NULL;
            }
            if (s_wifi_task_handle != NULL) {
                vTaskDelete(s_wifi_task_handle);
                s_wifi_task_handle = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        if (i == 0) s_supervisor_task_handle = handle;
        else if (i == 1) s_wifi_task_handle = handle;
        else if (i == 2) s_ui_task_handle = handle;
        else if (i == 3) szpi_preview_task_handle = handle;
        else if (i == 4) s_storage_task_handle = handle;
        else s_audio_task_handle = handle;
    }
    return ESP_OK;
}

esp_err_t szpi_app_runtime_start(const szpi_wifi_config_t *wifi_config, bool wifi_enabled, bool ui_enabled)
{
    if (s_runtime_started) return ESP_ERR_INVALID_STATE;
    bool have_config = wifi_config != NULL && wifi_config->ssid[0] != '\0';
    szpi_wifi_service_state_t initial_state = !wifi_enabled ? SZPI_WIFI_DISABLED : (have_config ? SZPI_WIFI_STOPPED : SZPI_WIFI_NO_CONFIG);
    ESP_RETURN_ON_ERROR(create_runtime_resources(initial_state), TAG, "create runtime resources");
    if (have_config) szpi_wifi_service_configure(wifi_config);
    s_ui_enabled = ui_enabled;
    esp_err_t err = create_tasks(wifi_enabled, have_config, ui_enabled);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < sizeof(s_resource_table) / sizeof(s_resource_table[0]); ++i) {
        ESP_LOGI(TAG, "resource=%s owner=%s capacity=%" PRIu32,
            s_resource_table[i].name, s_resource_table[i].owner, s_resource_table[i].capacity);
    }
    s_runtime_started = true;
    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_SYSTEM_READY | SZPI_EVENT_RUNTIME_START);
    if (wifi_enabled && have_config) {
        wifi_message_t message = {.kind = WIFI_MSG_START};
        if (xQueueSend(szpi_wifi_queue, &message, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
        szpi_runtime_record_queue_peaks();
    }
    return ESP_OK;
}

esp_err_t szpi_app_runtime_fault(esp_err_t cause)
{
    if (s_runtime_started) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(create_runtime_resources(SZPI_WIFI_DISABLED), TAG, "create fault diagnostics");
    esp_err_t err = create_tasks(false, false, false);
    if (err != ESP_OK) return err;
    s_runtime_started = true;
    xEventGroupSetBits(szpi_system_events, SZPI_EVENT_SYSTEM_FAULT | SZPI_EVENT_RUNTIME_START);
    uint32_t error_code = (uint32_t)cause;
    if (xQueueSend(szpi_supervisor_queue, &error_code, 0) == pdTRUE) szpi_runtime_record_queue_peaks();
    ESP_LOGE(TAG, "system fault latched: %s", esp_err_to_name(cause));
    return ESP_OK;
}

void szpi_supervisor_task(void *context)
{
    (void)context;
    (void)xEventGroupWaitBits(szpi_system_events, SZPI_EVENT_RUNTIME_START, pdFALSE, pdTRUE, portMAX_DELAY);
    uint32_t last_failures = 0;
    uint32_t diagnostic_count = 0;
    for (;;) {
        uint32_t error_code;
        BaseType_t got_error = xQueueReceive(szpi_supervisor_queue, &error_code, pdMS_TO_TICKS(10000));
        if (got_error == pdTRUE) diagnostic_count++;
        UBaseType_t wifi_peak, supervisor_peak;
        portENTER_CRITICAL(&s_peak_mux);
        wifi_peak = s_wifi_queue_peak;
        supervisor_peak = s_supervisor_queue_peak;
        portEXIT_CRITICAL(&s_peak_mux);
        uint32_t failures = 0;
        if (xSemaphoreTake(szpi_wifi_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            failures = szpi_wifi_status.state == SZPI_WIFI_FAILED ? 1U : 0U;
            xSemaphoreGive(szpi_wifi_status_lock);
        }
        if (got_error == pdTRUE || failures != last_failures || wifi_peak > 8 || supervisor_peak > 8) {
            ESP_LOGW(TAG, "runtime: internal heap=%" PRIu32 " min=%" PRIu32 " psram heap=%" PRIu32 " min=%" PRIu32 " wifi_queue_peak=%u supervisor_queue_peak=%u failures=%" PRIu32 " diagnostics=%" PRIu32 " code=%" PRIu32,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)wifi_peak, (unsigned)supervisor_peak, failures, diagnostic_count,
                got_error == pdTRUE ? error_code : 0U);
            last_failures = failures;
        } else {
            ESP_LOGI(TAG, "runtime: internal heap=%" PRIu32 " min=%" PRIu32 " psram heap=%" PRIu32 " min=%" PRIu32 " supervisor_stack=%u wifi_stack=%u ui_stack=%u wifi_queue_peak=%u supervisor_queue_peak=%u",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)uxTaskGetStackHighWaterMark(s_supervisor_task_handle) * sizeof(StackType_t),
                s_wifi_task_handle != NULL ? (unsigned)uxTaskGetStackHighWaterMark(s_wifi_task_handle) * sizeof(StackType_t) : 0U,
                s_ui_task_handle != NULL ? (unsigned)uxTaskGetStackHighWaterMark(s_ui_task_handle) * sizeof(StackType_t) : 0U,
                (unsigned)wifi_peak, (unsigned)supervisor_peak);
        }
    }
}

esp_err_t szpi_app_wifi_start(void)
{
    if (!s_runtime_started) return ESP_ERR_INVALID_STATE;
    szpi_wifi_status_t status;
    if (szpi_app_wifi_get_status(&status) != ESP_OK) return ESP_ERR_TIMEOUT;
    if (status.state == SZPI_WIFI_NO_CONFIG || status.state == SZPI_WIFI_DISABLED) return ESP_ERR_INVALID_STATE;
    if (status.state != SZPI_WIFI_STOPPED && status.state != SZPI_WIFI_FAILED) return ESP_ERR_INVALID_STATE;
    wifi_message_t message = {.kind = WIFI_MSG_START};
    if (xQueueSend(szpi_wifi_queue, &message, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    szpi_runtime_record_queue_peaks();
    return ESP_OK;
}

esp_err_t szpi_app_wifi_stop(TickType_t timeout_ticks)
{
    if (!s_runtime_started) return ESP_ERR_INVALID_STATE;
    szpi_wifi_status_t current;
    if (szpi_app_wifi_get_status(&current) != ESP_OK) return ESP_ERR_TIMEOUT;
    if (current.state == SZPI_WIFI_DISABLED || current.state == SZPI_WIFI_NO_CONFIG) return ESP_OK;
    TickType_t maximum_timeout = pdMS_TO_TICKS(3000);
    if (timeout_ticks > maximum_timeout) timeout_ticks = maximum_timeout;
    xEventGroupClearBits(szpi_system_events, SZPI_EVENT_WIFI_STOPPED);
    wifi_message_t message = {.kind = WIFI_MSG_STOP};
    TickType_t started = xTaskGetTickCount();
    if (xQueueSend(szpi_wifi_queue, &message, timeout_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    szpi_runtime_record_queue_peaks();
    TickType_t elapsed = xTaskGetTickCount() - started;
    TickType_t remaining = elapsed < timeout_ticks ? timeout_ticks - elapsed : 0;
    EventBits_t bits = xEventGroupWaitBits(szpi_system_events, SZPI_EVENT_WIFI_STOPPED, pdTRUE, pdTRUE, remaining);
    if ((bits & SZPI_EVENT_WIFI_STOPPED) == 0) return ESP_ERR_TIMEOUT;
    szpi_wifi_status_t status;
    if (szpi_app_wifi_get_status(&status) != ESP_OK) return ESP_ERR_TIMEOUT;
    return status.state == SZPI_WIFI_STOPPED ? ESP_OK : status.last_error;
}

esp_err_t szpi_app_wifi_retry(void)
{
    if (!s_runtime_started) return ESP_ERR_INVALID_STATE;
    szpi_wifi_status_t status;
    if (szpi_app_wifi_get_status(&status) != ESP_OK) return ESP_ERR_TIMEOUT;
    if (status.state == SZPI_WIFI_NO_CONFIG || status.state == SZPI_WIFI_DISABLED) return ESP_ERR_INVALID_STATE;
    if (status.state != SZPI_WIFI_STOPPED && status.state != SZPI_WIFI_FAILED) return ESP_ERR_INVALID_STATE;
    wifi_message_t message = {.kind = WIFI_MSG_RETRY};
    if (xQueueSend(szpi_wifi_queue, &message, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    szpi_runtime_record_queue_peaks();
    return ESP_OK;
}

esp_err_t szpi_app_wifi_get_status(szpi_wifi_status_t *status)
{
    if (status == NULL || szpi_wifi_status_lock == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(szpi_wifi_status_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *status = szpi_wifi_status;
    xSemaphoreGive(szpi_wifi_status_lock);
    if (status->state == SZPI_WIFI_ONLINE) {
        int8_t rssi;
        if (szpi_wifi_get_rssi(&rssi) == ESP_OK) { status->rssi = rssi; status->rssi_valid = true; }
    }
    return ESP_OK;
}

esp_err_t szpi_app_ui_start(void)
{
    if (!s_runtime_started || !s_ui_enabled || s_ui_task_handle == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(szpi_ui_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    szpi_ui_state_t state = szpi_ui_status.state;
    xSemaphoreGive(szpi_ui_status_lock);
    if (state != SZPI_UI_STOPPED && state != SZPI_UI_DISPLAY_FAULT && state != SZPI_UI_FAILED) return ESP_ERR_INVALID_STATE;
    xEventGroupClearBits(szpi_system_events, SZPI_EVENT_UI_STOPPED | SZPI_EVENT_DISPLAY_FAULT | SZPI_EVENT_TOUCH_FAULT);
    return xTaskNotify(s_ui_task_handle, SZPI_UI_CMD_START, eSetBits) == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t szpi_app_ui_stop(TickType_t timeout_ticks)
{
    if (!s_runtime_started || !s_ui_enabled || s_ui_task_handle == NULL) return ESP_ERR_INVALID_STATE;
    TickType_t maximum_timeout = pdMS_TO_TICKS(3000);
    if (timeout_ticks > maximum_timeout) timeout_ticks = maximum_timeout;
    if (xSemaphoreTake(szpi_ui_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    szpi_ui_state_t state = szpi_ui_status.state;
    xSemaphoreGive(szpi_ui_status_lock);
    if (state == SZPI_UI_STOPPED) return ESP_OK;
    xEventGroupClearBits(szpi_system_events, SZPI_EVENT_UI_STOPPED);
    if (xTaskNotify(s_ui_task_handle, SZPI_UI_CMD_STOP, eSetBits) != pdPASS) return ESP_FAIL;
    EventBits_t bits = xEventGroupWaitBits(szpi_system_events, SZPI_EVENT_UI_STOPPED, pdTRUE, pdTRUE, timeout_ticks);
    if ((bits & SZPI_EVENT_UI_STOPPED) == 0) return ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(szpi_ui_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    state = szpi_ui_status.state;
    esp_err_t last_error = szpi_ui_status.last_error;
    xSemaphoreGive(szpi_ui_status_lock);
    return state == SZPI_UI_STOPPED ? ESP_OK : (last_error != ESP_OK ? last_error : ESP_FAIL);
}

esp_err_t szpi_app_ui_get_status(szpi_ui_status_t *status)
{
    if (status == NULL || szpi_ui_status_lock == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(szpi_ui_status_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *status = szpi_ui_status;
    xSemaphoreGive(szpi_ui_status_lock);
    return ESP_OK;
}

esp_err_t szpi_app_camera_preview_start(void)
{
    if (!s_runtime_started || !s_ui_enabled || szpi_preview_task_handle == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(szpi_preview_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    szpi_camera_preview_state_t state = szpi_preview_status.state;
    bool frame_outstanding = szpi_preview_status.frame_outstanding;
    xSemaphoreGive(szpi_preview_status_lock);
    if (state != SZPI_CAMERA_PREVIEW_STOPPED && state != SZPI_CAMERA_PREVIEW_FAULT &&
        state != SZPI_CAMERA_PREVIEW_UNAVAILABLE) return ESP_ERR_INVALID_STATE;
    if (state == SZPI_CAMERA_PREVIEW_FAULT && frame_outstanding) return ESP_ERR_INVALID_STATE;
    xEventGroupClearBits(szpi_system_events, SZPI_EVENT_PREVIEW_STOPPED);
    return xTaskNotify(szpi_preview_task_handle, PREVIEW_CMD_START, eSetBits) == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t szpi_app_camera_preview_stop(TickType_t timeout_ticks)
{
    if (!s_runtime_started || szpi_preview_task_handle == NULL) return ESP_ERR_INVALID_STATE;
    if (timeout_ticks > pdMS_TO_TICKS(5000)) timeout_ticks = pdMS_TO_TICKS(5000);
    if (xSemaphoreTake(szpi_preview_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    szpi_camera_preview_state_t state = szpi_preview_status.state;
    bool frame_outstanding = szpi_preview_status.frame_outstanding;
    xSemaphoreGive(szpi_preview_status_lock);
    if (state == SZPI_CAMERA_PREVIEW_STOPPED || state == SZPI_CAMERA_PREVIEW_UNAVAILABLE ||
        (state == SZPI_CAMERA_PREVIEW_FAULT && !frame_outstanding)) return ESP_OK;
    xEventGroupClearBits(szpi_system_events, SZPI_EVENT_PREVIEW_STOPPED);
    if (xTaskNotify(szpi_preview_task_handle, PREVIEW_CMD_STOP, eSetBits) != pdPASS) return ESP_FAIL;
    EventBits_t bits = xEventGroupWaitBits(szpi_system_events, SZPI_EVENT_PREVIEW_STOPPED,
        pdTRUE, pdTRUE, timeout_ticks);
    if ((bits & SZPI_EVENT_PREVIEW_STOPPED) == 0) return ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(szpi_preview_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    state = szpi_preview_status.state;
    esp_err_t last_error = szpi_preview_status.last_error;
    xSemaphoreGive(szpi_preview_status_lock);
    return state == SZPI_CAMERA_PREVIEW_STOPPED ? ESP_OK : (last_error != ESP_OK ? last_error : ESP_FAIL);
}

esp_err_t szpi_app_camera_preview_request_stop(void)
{
    if (!s_runtime_started || szpi_preview_task_handle == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(szpi_preview_status_lock, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;
    szpi_camera_preview_state_t state = szpi_preview_status.state;
    bool frame_outstanding = szpi_preview_status.frame_outstanding;
    xSemaphoreGive(szpi_preview_status_lock);
    if (state == SZPI_CAMERA_PREVIEW_STOPPED || state == SZPI_CAMERA_PREVIEW_UNAVAILABLE ||
        (state == SZPI_CAMERA_PREVIEW_FAULT && !frame_outstanding)) return ESP_OK;
    return xTaskNotify(szpi_preview_task_handle, PREVIEW_CMD_STOP, eSetBits) == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t szpi_app_camera_preview_get_status(szpi_camera_preview_status_t *status)
{
    if (status == NULL || szpi_preview_status_lock == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(szpi_preview_status_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *status = szpi_preview_status;
    xSemaphoreGive(szpi_preview_status_lock);
    return ESP_OK;
}

EventGroupHandle_t szpi_app_get_system_events(void) { return szpi_system_events; }
