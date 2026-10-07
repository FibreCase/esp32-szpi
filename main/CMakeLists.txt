idf_component_register(SRCS "app_main.c" "app_boot.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES spi_flash esp_psram heap nvs_flash esp_partition app_update esp_app_format szpi_app szpi_board)
