idf_component_register(SRCS "src/szpi_runtime.c" "src/wifi_service.c" "src/web_service.c" "src/time_sync_service.c" "src/ui_service.c" "src/camera_preview_service.c" "src/storage_service.c" "src/audio_service.c" "src/ota_service.c" "src/szpi_wav.c"
                       INCLUDE_DIRS "include"
                       REQUIRES szpi_wifi szpi_display szpi_input szpi_camera szpi_audio szpi_storage freertos esp_common
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES szpi_board szpi_ui szpi_http esp_http_server espressif__cjson esp_wifi esp_event esp_netif nvs_flash esp_partition esp_psram spi_flash esp_timer espressif__mdns esp_app_format esp_https_ota mbedtls app_update)

set(dashboard_html "${CMAKE_CURRENT_LIST_DIR}/../../web/dist/index.html")
if(NOT EXISTS "${dashboard_html}")
    message(FATAL_ERROR "Build web assets first: cd web && npm ci && npm run build")
endif()
set(dashboard_gz "${CMAKE_CURRENT_BINARY_DIR}/dashboard.html.gz")
add_custom_command(OUTPUT "${dashboard_gz}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../szpi_wifi/assets/compress_portal.py"
            "${dashboard_html}" "${dashboard_gz}"
    DEPENDS "${dashboard_html}" "${CMAKE_CURRENT_LIST_DIR}/../szpi_wifi/assets/compress_portal.py"
    VERBATIM)
add_custom_target(szpi_dashboard_asset DEPENDS "${dashboard_gz}")
add_dependencies(${COMPONENT_LIB} szpi_dashboard_asset)
target_add_binary_data(${COMPONENT_LIB} "${dashboard_gz}" BINARY)
