idf_component_register(SRCS "src/szpi_board.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_spi freertos esp_common
                       PRIV_REQUIRES log)
