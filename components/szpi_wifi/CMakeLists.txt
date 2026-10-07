idf_component_register(SRCS "src/szpi_wifi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif esp_common
                       PRIV_REQUIRES esp_wifi esp_event nvs_flash lwip freertos)
