# SPDX-License-Identifier: MIT
# Both build systems generate the same small, shared system UI font family.
set(ASTER_SYSTEM_FONT "${CMAKE_CURRENT_BINARY_DIR}/aster_system_font.c")
set(_setup_root "${CMAKE_CURRENT_LIST_DIR}/../..")
file(GLOB _system_font_inputs "${_setup_root}/resources/fonts/ui_*.cpfont")
if(CMAKE_SCRIPT_MODE_FILE)
    return()
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(OUTPUT "${ASTER_SYSTEM_FONT}"
    COMMAND "${Python3_EXECUTABLE}" "${_setup_root}/tools/build_system_fonts.py" --output "${ASTER_SYSTEM_FONT}"
    DEPENDS "${_setup_root}/tools/build_system_fonts.py" "${_setup_root}/common/setup/ui-strings.txt"
        "${_setup_root}/common/setup/aster_setup_core.c" "${_setup_root}/systems/zephyr/ui/wifi_settings.c"
        "${_setup_root}/systems/zephyr/ui/qpanel.c" "${_setup_root}/systems/shared/setup/setup_ui.c" ${_system_font_inputs}
    VERBATIM)
add_custom_target(aster_system_fonts DEPENDS "${ASTER_SYSTEM_FONT}")
