idf_component_register(SRCS "src/szpi_camera.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common freertos
                       PRIV_REQUIRES esp32-camera esp_driver_gpio esp_driver_i2c esp_timer log szpi_board)
