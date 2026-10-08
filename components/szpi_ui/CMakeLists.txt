include(${CMAKE_CURRENT_LIST_DIR}/szpi_ui_sources.cmake)

idf_component_register(SRCS ${SZPI_UI_SOURCES}
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl)
