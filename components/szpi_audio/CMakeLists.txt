idf_component_register(SRCS "src/szpi_audio.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2s esp_codec_dev esp_common freertos
                       PRIV_REQUIRES szpi_board)
