#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "szpi_app.h"
#include "szpi_wifi.h"
#include "szpi_camera.h"

#define SZPI_EVENT_SYSTEM_READY (1U << 0)
#define SZPI_EVENT_NETWORK_READY (1U << 1)
#define SZPI_EVENT_SYSTEM_FAULT (1U << 2)
#define SZPI_EVENT_WIFI_STOPPED (1U << 3)
#define SZPI_EVENT_WIFI_OVERFLOW (1U << 4)
#define SZPI_EVENT_RUNTIME_START (1U << 5)
#define SZPI_EVENT_UI_READY (1U << 6)
#define SZPI_EVENT_DISPLAY_FAULT (1U << 7)
#define SZPI_EVENT_TOUCH_FAULT (1U << 8)
#define SZPI_EVENT_UI_STOPPED (1U << 9)
#define SZPI_EVENT_UI_START (1U << 10)
#define SZPI_EVENT_PREVIEW_STOPPED (1U << 11)
#define SZPI_UI_CMD_STOP (1U << 0)
#define SZPI_UI_CMD_START (1U << 1)

typedef enum { WIFI_MSG_START, WIFI_MSG_STOP, WIFI_MSG_RETRY, WIFI_MSG_EVENT, WIFI_MSG_DIAGNOSTIC } wifi_message_kind_t;
typedef struct {
    wifi_message_kind_t kind;
    TaskHandle_t waiter;
    szpi_wifi_event_t event;
} wifi_message_t;

extern QueueHandle_t szpi_wifi_queue;
extern QueueHandle_t szpi_supervisor_queue;
extern EventGroupHandle_t szpi_system_events;
extern SemaphoreHandle_t szpi_wifi_status_lock;
extern szpi_wifi_status_t szpi_wifi_status;
extern SemaphoreHandle_t szpi_ui_status_lock;
extern szpi_ui_status_t szpi_ui_status;
extern QueueHandle_t szpi_preview_frame_queue;
extern QueueHandle_t szpi_preview_ack_queue;
extern SemaphoreHandle_t szpi_preview_status_lock;
extern szpi_camera_preview_status_t szpi_preview_status;
extern TaskHandle_t szpi_preview_task_handle;
typedef struct {
    szpi_camera_frame_t frame;
    uint32_t generation;
} szpi_preview_frame_message_t;
typedef struct { uint32_t token; uint32_t generation; } szpi_preview_ack_message_t;
void szpi_camera_preview_service_task(void *context);
void szpi_wifi_service_task(void *context);
void szpi_ui_service_task(void *context);
void szpi_wifi_service_configure(const szpi_wifi_config_t *config);
void szpi_supervisor_task(void *context);
void szpi_wifi_post_event(const szpi_wifi_event_t *event, void *context);
void szpi_runtime_record_queue_peaks(void);
