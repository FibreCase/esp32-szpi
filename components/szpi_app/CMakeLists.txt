idf_component_register(SRCS "src/szpi_runtime.c" "src/wifi_service.c" "src/ui_service.c" "src/camera_preview_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES szpi_wifi szpi_display szpi_input szpi_camera freertos esp_common
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES szpi_board lvgl esp_event esp_netif nvs_flash esp_partition esp_psram spi_flash esp_timer)
