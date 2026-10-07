idf_component_register(SRCS "src/szpi_display.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common freertos lvgl
                       PRIV_REQUIRES esp_driver_ledc esp_driver_spi esp_lcd esp_timer log szpi_board)
