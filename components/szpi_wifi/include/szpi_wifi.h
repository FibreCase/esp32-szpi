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
    bool wpa3_only;
} szpi_wifi_config_t;

typedef enum {
    SZPI_WIFI_EVENT_STARTED,
    SZPI_WIFI_EVENT_CONNECTED,
    SZPI_WIFI_EVENT_DISCONNECTED,
    SZPI_WIFI_EVENT_GOT_IP,
    SZPI_WIFI_EVENT_LOST_IP,
    SZPI_WIFI_EVENT_STOPPED,
    SZPI_WIFI_EVENT_OVERFLOW,
    SZPI_WIFI_EVENT_DPP_URI,
    SZPI_WIFI_EVENT_DPP_CONFIG,
    SZPI_WIFI_EVENT_DPP_FAILED,
    SZPI_WIFI_EVENT_SCAN_DONE,
} szpi_wifi_event_id_t;

typedef struct {
    szpi_wifi_event_id_t id;
    int disconnect_reason;
    esp_netif_ip_info_t ip_info;
    uint32_t generation;
    szpi_wifi_config_t config;
    char uri[512];
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

typedef struct {
    bool mac_valid;
    uint8_t mac[6];
    bool link_valid;
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
} szpi_wifi_link_info_t;
// Read-only driver snapshot; link_valid is false if not associated.
esp_err_t szpi_wifi_get_link_info(szpi_wifi_link_info_t *info);

#define SZPI_WIFI_SCAN_MAX 16

typedef enum {
    SZPI_PROV_IDLE, SZPI_PROV_ACTIVE, SZPI_PROV_TESTING,
    SZPI_PROV_FAILED, SZPI_PROV_SUCCESS, SZPI_PROV_ERROR,
} szpi_provision_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool supported;
    bool wpa3_only;
} szpi_wifi_scan_entry_t;

typedef struct {
    szpi_provision_state_t state;
    uint32_t generation;
    uint32_t request_id;
    bool active;
    bool dpp_ready;
    bool scanning;
    char ap_ssid[33];
    char ap_password[17];
    char wifi_qr[160];
    char dpp_uri[512];
    char message[96];
    uint8_t scan_count;
    szpi_wifi_scan_entry_t scan[SZPI_WIFI_SCAN_MAX];
} szpi_provision_status_t;

typedef enum { SZPI_PORTAL_SUBMIT, SZPI_PORTAL_SCAN } szpi_portal_request_t;
typedef esp_err_t (*szpi_portal_request_cb_t)(szpi_portal_request_t request,
    uint32_t generation, const szpi_wifi_config_t *config);
typedef esp_err_t (*szpi_portal_status_cb_t)(szpi_provision_status_t *status);

/* Wi-Fi owner task only; event callbacks copy data before returning. init may
 * receive NULL config for first setup. NVS uses one versioned blob and commit;
 * failed writes never erase the previous configuration. */
esp_err_t szpi_wifi_load_config(szpi_wifi_config_t *config);
esp_err_t szpi_wifi_save_config(const szpi_wifi_config_t *config);
esp_err_t szpi_wifi_forget_config(void);
esp_err_t szpi_wifi_set_station(const szpi_wifi_config_t *config);
esp_err_t szpi_wifi_verify_station(const szpi_wifi_config_t *config);
esp_err_t szpi_wifi_provision_start(szpi_provision_status_t *status, bool use_dpp,
    szpi_portal_request_cb_t request_cb, szpi_portal_status_cb_t status_cb);
esp_err_t szpi_wifi_provision_stop(void);
esp_err_t szpi_wifi_dpp_stop(void);
esp_err_t szpi_wifi_dpp_listen(void);
esp_err_t szpi_wifi_dpp_pause(void);
esp_err_t szpi_wifi_scan_start(void);
esp_err_t szpi_wifi_scan_results(szpi_provision_status_t *status);
/* Bounded nonblocking DNS work, called every <=100ms by the owner task. */
void szpi_wifi_portal_poll(void);
