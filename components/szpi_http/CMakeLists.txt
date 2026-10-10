idf_component_register(SRCS "src/szpi_http.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server esp_common
                       PRIV_REQUIRES freertos)
