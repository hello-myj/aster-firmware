set(UI "${CMAKE_CURRENT_LIST_DIR}/../../../zephyr/ui")
include(${CMAKE_CURRENT_LIST_DIR}/../../../../common/sources.cmake)
set(SETUP "${CMAKE_CURRENT_LIST_DIR}/../../../../third_party/qrcodegen")
idf_build_get_property(ai_enabled ASTER_AI_ENABLED)
idf_build_get_property(ai_avatar_enabled ASTER_AI_AVATAR)
if(ai_enabled)
    set(ai_component aster_ai)
endif()
idf_component_register(SRCS ${ASTER_COMMON_POWER_UI} "ui.c" "platform.c" "wifi.c" "${UI}/launcher/flux.c" "${UI}/launcher/flux_font.c"
    ${ASTER_COMMON_DEVICE_SOURCES} ${ASTER_COMMON_OS_SOURCES} "../../port/device.c" "../../port/os.c"
    ${ASTER_COMMON_SERVICE_SOURCES} "../../../shared/port/services.c"
    ${ASTER_COMMON_PORT_CHECK}
    ${ASTER_COMMON_DEPLOY} "../../../shared/port/deploy_ui.c"
    ${ASTER_COMMON_NETWORK_PORT} "../../../shared/port/network.c"
    "../../../../platform/esp32s3/touch_ui.c" "${UI}/gesture.c" "${UI}/qpanel.c" "${UI}/wifi_settings.c" "${UI}/ui_splash.c"
    INCLUDE_DIRS "." "${UI}/launcher" "../../../../platform/esp32s3"
    REQUIRES aster_runtime aster_render aster_audio aster_connectivity esp_driver_ledc ${ai_component})
if(ai_enabled)
    target_sources(${COMPONENT_LIB} PRIVATE "ai_voice.c" "ai_dialog.c" "ai_reply_audio.c" "ai_tts_stream.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ASTER_AI_ENABLED=1)
endif()
if(ai_avatar_enabled)
    target_sources(${COMPONENT_LIB} PRIVATE "ai_avatar.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ASTER_AI_AVATAR=1)
endif()
target_sources(${COMPONENT_LIB} PRIVATE "${UI}/../../shared/setup/setup_ui.c" "${SETUP}/qrcodegen.c")
target_include_directories(${COMPONENT_LIB} PRIVATE "${SETUP}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_ASTER_FIRST_BOOT=1 CONFIG_ASTER_LAUNCHER_FLUX=1 CONFIG_ASTER_WIFI=1
    CONFIG_ASTER_FLUX_ART_CACHE=8
    )
if(NOT CONFIG_ASTER_BOARD_ID OR NOT CONFIG_ASTER_DISPLAY_WIDTH OR NOT CONFIG_ASTER_DISPLAY_HEIGHT)
    message(FATAL_ERROR "Board must declare ASTER_BOARD_ID and its own display geometry")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_ASTER_DISPLAY_WIDTH=${CONFIG_ASTER_DISPLAY_WIDTH}
    CONFIG_ASTER_DISPLAY_HEIGHT=${CONFIG_ASTER_DISPLAY_HEIGHT}
    "CONFIG_ASTER_BOARD_ID=\"${CONFIG_ASTER_BOARD_ID}\"")
