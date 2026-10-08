# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32-szpi)

# Keep LVGL's static 64 KiB object pool in PSRAM; LCD DMA buffers remain internal.
# Apply only to the IDF LVGL target, leaving the shared sources and simulator portable.
idf_component_get_property(szpi_lvgl_target lvgl__lvgl COMPONENT_LIB)
idf_component_get_property(szpi_lvgl_dir lvgl__lvgl COMPONENT_DIR)
set_property(SOURCE "${szpi_lvgl_dir}/src/stdlib/builtin/lv_mem_core_builtin.c"
             TARGET_DIRECTORY ${szpi_lvgl_target}
             APPEND PROPERTY COMPILE_OPTIONS "-include" "esp_attr.h")
target_compile_definitions(${szpi_lvgl_target} PRIVATE LV_ATTRIBUTE_LARGE_RAM_ARRAY=EXT_RAM_BSS_ATTR)
