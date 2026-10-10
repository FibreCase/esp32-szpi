idf_component_register(SRCS "src/szpi_runtime.c" "src/wifi_service.c" "src/ui_service.c" "src/camera_preview_service.c" "src/storage_service.c" "src/audio_service.c" "src/ota_service.c" "src/szpi_wav.c"
                       INCLUDE_DIRS "include"
                       REQUIRES szpi_wifi szpi_display szpi_input szpi_camera szpi_audio szpi_storage freertos esp_common
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES szpi_board szpi_ui esp_wifi esp_event esp_netif nvs_flash esp_partition esp_psram spi_flash esp_timer espressif__mdns esp_app_format esp_https_ota mbedtls app_update)
