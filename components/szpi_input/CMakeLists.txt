idf_component_register(SRCS "src/szpi_input.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_i2c esp_timer freertos log szpi_board)
