set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
set(UI "${ROOT}/systems/zephyr/ui")
set(IMG "${ROOT}/modules/imgdec")
file(GLOB_RECURSE LV_SOURCES "${ROOT}/third_party/lvgl/src/*.c")
list(FILTER LV_SOURCES EXCLUDE REGEX "/(examples|tests|demos|libs)/")
list(FILTER LV_SOURCES EXCLUDE REGEX "/stdlib/(uefi|rtthread|micropython)/")
idf_component_register(SRCS "port.c" "ppa_fill.c"
    "${UI}/ui_gfx.c" "${UI}/pp_gfx.c" "${UI}/ui_font.c" "${UI}/font_sd.c" "${UI}/flux_draw.c"
    "${IMG}/imgdec.c" "${IMG}/imgdec_jpg.c" "${IMG}/imgdec_png.c"
    "${ROOT}/third_party/lvgl/src/libs/bin_decoder/lv_bin_decoder.c"
    "${ROOT}/third_party/lvgl/src/libs/rle/lv_rle.c"
    "${IMG}/vendor/tjpgd.c" "${IMG}/vendor/miniz.c" ${LV_SOURCES}
    INCLUDE_DIRS "." "compat" "${UI}" "${ROOT}/contract/include" "${ROOT}/platform"
      "${ROOT}/systems/zephyr/core" "${ROOT}/systems/zephyr/services"
      "${ROOT}/modules/prettypoly" "${IMG}" "${ROOT}/third_party/lvgl" "${ROOT}/third_party/lvgl/src"
    REQUIRES esp_timer heap log esp_driver_ppa)
target_compile_definitions(${COMPONENT_LIB} PUBLIC ASTER_SYSTEM_ESP_IDF=1 CONFIG_ASTER_LAUNCHER_FLUX=1 LV_LVGL_H_INCLUDE_SIMPLE
    "LV_CONF_PATH=\"${UI}/lv_conf.h\"" CONFIG_ASTER_LVGL_MEM_KB=256
    CONFIG_ASTER_IMGDEC_JPG=1 CONFIG_ASTER_IMGDEC_PNG=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -include "${CMAKE_CURRENT_LIST_DIR}/compat/zephyr/sys/util.h"
    -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO -DMINIZ_NO_TIME
    -Wno-error -Wno-unused-function -Wno-unused-variable)
