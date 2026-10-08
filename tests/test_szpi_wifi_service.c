/* Executes production service transitions with deterministic Wi-Fi/NVS faults.
 * No mock radio result is presented as hardware acceptance. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "szpi_app.h"
#define SZPI_RUNTIME_INTERNAL_H
#define SZPI_EVENT_RUNTIME_START 1
#define SZPI_EVENT_TIME_SYNCED 2
#define SZPI_EVENT_NETWORK_READY 4
#define SZPI_EVENT_WIFI_OVERFLOW 8
#define SZPI_EVENT_WIFI_STOPPED 16
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xFFFFFFFFU
typedef enum { WIFI_MSG_START, WIFI_MSG_STOP, WIFI_MSG_RETRY, WIFI_MSG_EVENT, WIFI_MSG_DIAGNOSTIC, WIFI_MSG_PROVISION, WIFI_MSG_CANCEL, WIFI_MSG_FORGET, WIFI_MSG_SUBMIT, WIFI_MSG_SCAN } wifi_message_kind_t;
typedef struct {
    wifi_message_kind_t kind;
    void *waiter;
    uint32_t generation;
    bool use_dpp;
    uint32_t request_id;
    szpi_wifi_config_t config;
    szpi_wifi_event_t event;
} wifi_message_t;
static wifi_message_t last_queued_message;
static void *szpi_wifi_queue, *szpi_wifi_status_lock, *szpi_system_events;
static szpi_wifi_status_t szpi_wifi_status;
static unsigned bits, queue_count, connect_count, save_count, stop_count;
static int64_t now = 1000000;
static esp_err_t save_result, verify_result, stop_result;
static bool linked;
static unsigned dpp_listen_count, dpp_pause_count, provision_start_count;
static bool last_use_dpp;
static szpi_wifi_config_t stored, applied;
static int xSemaphoreTake(void *lock, unsigned ticks) { (void)lock; (void)ticks; return pdTRUE; }
static void xSemaphoreGive(void *lock) { (void)lock; }
static void xEventGroupSetBits(void *group, unsigned value) { (void)group; bits |= value; }
static void xEventGroupClearBits(void *group, unsigned value) { (void)group; bits &= ~value; }
static unsigned xEventGroupGetBits(void *group) { (void)group; return bits; }
static unsigned xEventGroupWaitBits(void *group, unsigned value, int clear, int all, unsigned wait) { (void)group; (void)value; (void)clear; (void)all; (void)wait; return bits; }
static int xQueueSend(void *queue, const void *message, unsigned ticks) { (void)queue; (void)ticks; memcpy(&last_queued_message, message, sizeof(last_queued_message)); queue_count++; return pdTRUE; }
static int xQueueReceive(void *queue, void *message, unsigned ticks) { (void)queue; (void)message; (void)ticks; return pdFALSE; }
static unsigned uxTaskGetStackHighWaterMark(void *task) { (void)task; return 1234; }
static void szpi_runtime_record_queue_peaks(void) {}
#include "../components/szpi_app/src/wifi_service.c"

int64_t esp_timer_get_time(void) { return now; }
static unsigned sntp_init_count, sntp_restart_count;
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) { assert(config->sync_cb); sntp_init_count++; return ESP_OK; }
esp_err_t esp_netif_sntp_start(void) { sntp_restart_count++; return ESP_OK; }
esp_err_t szpi_wifi_init(const szpi_wifi_config_t *c, szpi_wifi_event_sink_t sink, void *context) { (void)c; (void)sink; (void)context; return ESP_OK; }
esp_err_t szpi_wifi_start(void) { return ESP_OK; }
esp_err_t szpi_wifi_stop(void) { return ESP_OK; }
esp_err_t szpi_wifi_connect(void) { connect_count++; return ESP_OK; }
esp_err_t szpi_wifi_disconnect(void) { return linked ? ESP_OK : ESP_ERR_WIFI_NOT_CONNECT; }
esp_err_t szpi_wifi_set_station(const szpi_wifi_config_t *config) { applied = *config; return ESP_OK; }
esp_err_t szpi_wifi_get_rssi(int8_t *rssi) { *rssi = -40; return linked ? ESP_OK : ESP_ERR_WIFI_NOT_CONNECT; }
esp_err_t szpi_wifi_verify_station(const szpi_wifi_config_t *config) { (void)config; return verify_result; }
esp_err_t szpi_wifi_load_config(szpi_wifi_config_t *config) { *config = stored; return stored.ssid[0] ? ESP_OK : ESP_ERR_NVS_NOT_FOUND; }
esp_err_t szpi_wifi_save_config(const szpi_wifi_config_t *config) { save_count++; if (!save_result) stored = *config; return save_result; }
esp_err_t szpi_wifi_forget_config(void) { memset(&stored, 0, sizeof(stored)); return ESP_OK; }
esp_err_t szpi_wifi_provision_start(szpi_provision_status_t *status, bool use_dpp, szpi_portal_request_cb_t request, szpi_portal_status_cb_t get) { (void)status; (void)request; (void)get; provision_start_count++; last_use_dpp = use_dpp; return ESP_OK; }
esp_err_t szpi_wifi_provision_stop(void) { stop_count++; return stop_result; }
esp_err_t szpi_wifi_dpp_stop(void) { return ESP_OK; }
esp_err_t szpi_wifi_dpp_pause(void) { dpp_pause_count++; return ESP_OK; }
esp_err_t szpi_wifi_dpp_listen(void) { dpp_listen_count++; return ESP_OK; }
esp_err_t szpi_wifi_scan_start(void) { return ESP_OK; }
esp_err_t szpi_wifi_scan_results(szpi_provision_status_t *status) { (void)status; return ESP_OK; }
void szpi_wifi_portal_poll(void) {}

static void setup(void)
{
    s_saved = stored = (szpi_wifi_config_t){.ssid = "Original", .password = "old_password"};
    s_have_config = s_initialized = s_started = true;
    s_prov = (szpi_provision_status_t){.active = true, .generation = 2, .state = SZPI_PROV_ACTIVE};
    s_testing = s_disconnect_pending = s_link_seen = false;
    s_accept_dpp = true; s_dpp_selected = false;
    dpp_listen_count = dpp_pause_count = provision_start_count = 0;
    save_result = verify_result = stop_result = ESP_OK;
    save_count = connect_count = stop_count = 0;
    linked = false;
    publish();
}

int main(void)
{
    const szpi_wifi_config_t candidate = {.ssid = "New network", .password = "new_password"};
    szpi_wifi_event_t connected = {.id = SZPI_WIFI_EVENT_CONNECTED};
    szpi_wifi_event_t ip = {.id = SZPI_WIFI_EVENT_GOT_IP, .ip_info.ip.addr = 1};
    setup();
    szpi_wifi_event_t uri = {.id = SZPI_WIFI_EVENT_DPP_URI, .generation = 2, .uri = "DPP:test;;"};
    szpi_wifi_post_event(&uri, NULL);
    assert(last_queued_message.event.generation == 2 && !strcmp(last_queued_message.event.uri, "DPP:test;;"));
    assert(szpi_wifi_status.sys_evt_stack_min_bytes == 1234);
    submit_candidate(&candidate);
    assert(s_testing && connect_count == 1 && !strcmp(stored.ssid, "Original"));
    event_received(&ip); /* Stale DHCP event without current link cannot commit. */
    assert(save_count == 0);
    event_received(&connected);
    verify_result = ESP_ERR_INVALID_STATE;
    event_received(&ip); /* Wrong station or no real IP cannot commit. */
    assert(save_count == 0);
    verify_result = ESP_OK;
    event_received(&ip);
    assert(save_count == 1 && !strcmp(stored.ssid, "New network"));
    assert(s_prov.active && s_prov.state == SZPI_PROV_SUCCESS && stop_count == 0);
    event_received(&ip); /* Duplicate IP must not write twice. */
    assert(save_count == 1);
    assert(sntp_init_count == 0); /* Wait until provisioning radio teardown. */
    close_session(true);
    assert(sntp_init_count == 1);
    start_time_sync();
    assert(sntp_restart_count == 1);
    assert(!s_prov.active && s_state == SZPI_WIFI_ONLINE && stop_count == 1);
    setup();
    submit_candidate(&candidate);
    event_received(&connected);
    save_result = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    event_received(&ip);
    assert(s_prov.active && s_prov.state == SZPI_PROV_FAILED);
    assert(!s_testing && !strcmp(stored.ssid, "Original"));
    close_session(true);
    assert(!strcmp(applied.ssid, "Original"));
    setup();
    submit_candidate(&candidate);
    test_failed(ESP_ERR_TIMEOUT);
    assert(s_prov.active && save_count == 0 && !strcmp(stored.ssid, "Original"));
    setup();
    submit_candidate(&candidate);
    close_session(true); /* Cancel while testing restores original credentials. */
    assert(save_count == 0 && !strcmp(stored.ssid, "Original") && !strcmp(applied.ssid, "Original"));
    setup();
    assert(portal_request(SZPI_PORTAL_SUBMIT, 1, &candidate) == ESP_ERR_INVALID_STATE);
    assert(portal_request(SZPI_PORTAL_SUBMIT, 2, &candidate) == ESP_OK);
    assert(portal_request(SZPI_PORTAL_SUBMIT, 2, &candidate) == ESP_ERR_INVALID_STATE);
    setup();
    stop_result = ESP_FAIL;
    close_session(true); /* Teardown failure must retain ownership. */
    assert(s_prov.active && s_prov.state == SZPI_PROV_ERROR);
    setup();
    linked = true;
    submit_candidate(&candidate);
    assert(s_disconnect_pending && connect_count == 0);
    event_received(&ip);
    assert(save_count == 0);
    linked = false;
    szpi_wifi_event_t disconnected = {.id = SZPI_WIFI_EVENT_DISCONNECTED};
    event_received(&disconnected);
    assert(!s_disconnect_pending && connect_count == 1);
    setup();
    submit_candidate(&candidate);
    disconnected.disconnect_reason = WIFI_REASON_NO_AP_FOUND;
    event_received(&disconnected);
    assert(!s_testing && s_prov.state == SZPI_PROV_FAILED && save_count == 0);
    assert(strstr(s_prov.message, "Network not found") && !strcmp(stored.ssid, "Original"));
    setup();
    s_prov.active = false;
    s_have_config = false;
    saved_round();
    assert(provision_start_count == 0 && s_state == SZPI_WIFI_NO_CONFIG);
    begin_session(true, 10);
    assert(provision_start_count == 1 && last_use_dpp && s_prov.request_id == 10);
    uri.generation = s_prov.generation;
    event_received(&uri);
    assert(dpp_listen_count == 1 && s_prov.dpp_ready);
    begin_session(false, 11);
    assert(provision_start_count == 2 && !last_use_dpp && s_prov.request_id == 11);
    assert(!s_prov.dpp_ready && !s_accept_dpp);
    szpi_wifi_event_t config_event = {.id = SZPI_WIFI_EVENT_DPP_CONFIG, .generation = s_prov.generation, .config = candidate};
    event_received(&config_event);
    assert(connect_count == 0 && !s_testing);
    puts("Wi-Fi service transitions: PASS");
}
