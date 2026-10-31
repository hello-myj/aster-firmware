set(UI "${CMAKE_CURRENT_LIST_DIR}/../../../zephyr/ui")
idf_component_register(SRCS "ui.c" "platform.c" "wifi.c" "${UI}/launcher/flux.c" "${UI}/launcher/flux_font.c"
    "../../../../platform/esp32s3/touch_ui.c" "${UI}/gesture.c" "${UI}/qpanel.c" "${UI}/wifi_settings.c" "${UI}/ui_splash.c"
    INCLUDE_DIRS "." "${UI}/launcher" "../../../../platform/esp32s3"
    REQUIRES aster_runtime aster_render aster_audio aster_connectivity esp_driver_ledc)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_ASTER_LAUNCHER_FLUX=1 CONFIG_ASTER_WIFI=1
    CONFIG_ASTER_FLUX_ART_CACHE=8
    CONFIG_ASTER_DISPLAY_WIDTH=480 CONFIG_ASTER_DISPLAY_HEIGHT=480
    "CONFIG_ASTER_BOARD_ID=\"osptek-p4-st7102\"")
