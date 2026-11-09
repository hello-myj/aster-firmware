cmake_minimum_required(VERSION 3.20)
project(aster_setup_preview C)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
include(${ROOT}/common/setup/assets.cmake)
file(GLOB_RECURSE LV_SOURCES "${ROOT}/third_party/lvgl/src/*.c")
list(FILTER LV_SOURCES EXCLUDE REGEX "/(examples|tests|demos|libs)/")
list(FILTER LV_SOURCES EXCLUDE REGEX "/stdlib/(uefi|rtthread|micropython)/")
add_executable(setup-preview main.c "${ROOT}/systems/shared/setup/setup_ui.c"
    "${ROOT}/common/setup/aster_setup_core.c" "${ROOT}/third_party/qrcodegen/qrcodegen.c"
    "${ASTER_SYSTEM_FONT}" ${LV_SOURCES}
    "${ROOT}/third_party/lvgl/src/libs/bin_decoder/lv_bin_decoder.c"
    "${ROOT}/third_party/lvgl/src/libs/rle/lv_rle.c")
add_dependencies(setup-preview aster_system_fonts)
target_include_directories(setup-preview PRIVATE . "${ROOT}/systems/zephyr/core" "${ROOT}/systems/zephyr/ui"
    "${ROOT}/systems/zephyr/ui/launcher"
    "${ROOT}/common/setup" "${ROOT}/common/include" "${ROOT}/contract/include" "${ROOT}/platform"
    "${ROOT}/third_party/qrcodegen" "${ROOT}/third_party/lvgl")
target_compile_definitions(setup-preview PRIVATE CONFIG_ASTER_LVGL_MEM_KB=256
    LV_CONF_PATH="${ROOT}/systems/zephyr/ui/lv_conf.h" LV_LVGL_H_INCLUDE_SIMPLE)
target_link_libraries(setup-preview PRIVATE m)
