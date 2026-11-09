# SPDX-License-Identifier: MIT
# Shared source inventory. Build-system and OS decisions belong to callers.
set(ASTER_COMMON_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(ASTER_COMMON_INCLUDE_DIRS
    "${ASTER_COMMON_ROOT}/include"
    "${ASTER_COMMON_ROOT}/package"
    "${ASTER_COMMON_ROOT}/net"
    "${ASTER_COMMON_ROOT}/setup")
set(ASTER_COMMON_SETUP_SOURCES "${ASTER_COMMON_ROOT}/setup/aster_setup_core.c")
set(ASTER_COMMON_PACKAGE_SOURCES "${ASTER_COMMON_ROOT}/package/aster_pkg_decode.c")
set(ASTER_COMMON_HTTP_SOURCES "${ASTER_COMMON_ROOT}/net/aster_http_codec.c")
set(ASTER_COMMON_MQTT_SOURCES "${ASTER_COMMON_ROOT}/net/aster_mqtt_codec.c")
set(ASTER_COMMON_WS_SOURCES "${ASTER_COMMON_ROOT}/net/aster_ws_codec.c")
