cmake_minimum_required(VERSION 3.22)
project(szpi_ui_sim C)

include(FetchContent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "LVGL config")
set(LV_BUILD_LVGL_H_SIMPLE_INCLUDE ON CACHE BOOL "Use simple LVGL include")
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "Disable LVGL examples")
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "Disable LVGL demos")

FetchContent_Declare(lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v9.5.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lvgl)

include(${CMAKE_CURRENT_SOURCE_DIR}/../components/szpi_ui/szpi_ui_sources.cmake)
add_executable(szpi_ui_sim main.c mock_backend.c ${SZPI_UI_SOURCES})
target_include_directories(szpi_ui_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/szpi_ui/include
    ${lvgl_SOURCE_DIR}
)
target_link_libraries(szpi_ui_sim PRIVATE lvgl PkgConfig::SDL2)
target_compile_features(szpi_ui_sim PRIVATE c_std_11)
