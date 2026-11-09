# SPDX-License-Identifier: MIT
include(${CMAKE_CURRENT_LIST_DIR}/../sources.cmake)
set(ASTER_SETUP_PAGE "${CMAKE_CURRENT_BINARY_DIR}/aster_setup_page.c")
set(_setup_root "${CMAKE_CURRENT_LIST_DIR}/../..")
if(NOT CMAKE_SCRIPT_MODE_FILE)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(OUTPUT "${ASTER_SETUP_PAGE}"
    COMMAND "${Python3_EXECUTABLE}" "${_setup_root}/tools/embed_setup_page.py"
        "${_setup_root}/common/setup/provision.html" "${ASTER_SETUP_PAGE}"
    DEPENDS "${_setup_root}/tools/embed_setup_page.py" "${_setup_root}/common/setup/provision.html" VERBATIM)
add_custom_target(aster_setup_page DEPENDS "${ASTER_SETUP_PAGE}")
endif()
set(ASTER_SETUP_SERVICE_SOURCES ${ASTER_COMMON_SETUP_SOURCES}
    "${_setup_root}/common/setup/aster_setup_json.c"
    "${_setup_root}/systems/shared/setup/aster_setup.c"
    "${_setup_root}/systems/shared/setup/aster_setup_web.c" "${ASTER_SETUP_PAGE}")
