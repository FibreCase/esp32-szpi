idf_component_register(SRCS "src/szpi_wifi.c" "src/wifi_credentials.c"
                            "src/wifi_portal.c" "src/provision_protocol.c" "src/wifi_ap_netif.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif esp_common
                       PRIV_REQUIRES esp_wifi esp_event nvs_flash lwip freertos
                                     wpa_supplicant esp_timer esp_http_server espressif__cjson)

# Reproducible single-file portal compressed at build time; no flash filesystem.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(portal_gz "${CMAKE_CURRENT_BINARY_DIR}/portal.html.gz")
add_custom_command(OUTPUT "${portal_gz}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/assets/compress_portal.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/portal.html" "${portal_gz}"
    DEPENDS "assets/portal.html" "assets/compress_portal.py"
    VERBATIM)
add_custom_target(szpi_portal_asset DEPENDS "${portal_gz}")
add_dependencies(${COMPONENT_LIB} szpi_portal_asset)
target_add_binary_data(${COMPONENT_LIB} "${portal_gz}" BINARY)
