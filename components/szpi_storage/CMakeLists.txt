idf_component_register(SRCS "src/szpi_storage.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_sdmmc fatfs sdmmc esp_common
                       PRIV_REQUIRES szpi_board)
