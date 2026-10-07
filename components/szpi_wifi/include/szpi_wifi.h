#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif_types.h"

#define SZPI_WIFI_SSID_MAX 32
#define SZPI_WIFI_PASSWORD_MAX 64

typedef struct {
    char ssid[SZPI_WIFI_SSID_MAX + 1];
    char password[SZPI_WIFI_PASSWORD_MAX + 1];
} szpi_wifi_config_t;

typedef enum {
    SZPI_WIFI_EVENT_STARTED,
    SZPI_WIFI_EVENT_CONNECTED,
    SZPI_WIFI_EVENT_DISCONNECTED,
    SZPI_WIFI_EVENT_GOT_IP,
    SZPI_WIFI_EVENT_LOST_IP,
    SZPI_WIFI_EVENT_STOPPED,
    SZPI_WIFI_EVENT_OVERFLOW,
} szpi_wifi_event_id_t;

typedef struct {
    szpi_wifi_event_id_t id;
    int disconnect_reason;
    esp_netif_ip_info_t ip_info;
} szpi_wifi_event_t;

typedef void (*szpi_wifi_event_sink_t)(const szpi_wifi_event_t *event, void *context);

// Init owns the default STA netif, driver and event handlers. Callback must be
// nonblocking and remains valid until deinit. No project task is created here.
esp_err_t szpi_wifi_init(const szpi_wifi_config_t *config, szpi_wifi_event_sink_t sink, void *context);
esp_err_t szpi_wifi_start(void);
esp_err_t szpi_wifi_connect(void);
esp_err_t szpi_wifi_disconnect(void);
esp_err_t szpi_wifi_stop(void);
esp_err_t szpi_wifi_deinit(void);
esp_err_t szpi_wifi_get_rssi(int8_t *rssi);
