idf_component_register(SRCS "src/szpi_wifi.c" "src/wifi_credentials.c"
                            "src/wifi_portal.c" "src/provision_protocol.c" "src/wifi_ap_netif.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif esp_common
                       PRIV_REQUIRES esp_wifi esp_event nvs_flash lwip freertos
                                     wpa_supplicant esp_timer esp_http_server espressif__cjson szpi_http)
