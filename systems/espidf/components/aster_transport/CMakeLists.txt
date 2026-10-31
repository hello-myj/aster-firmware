set(SVC "${CMAKE_CURRENT_LIST_DIR}/../../../zephyr/services")
idf_component_register(SRCS "port.c" "${SVC}/aster_net.c" "${SVC}/aster_net_event.c"
    "${SVC}/aster_http.c" "${SVC}/aster_http_codec.c" "${SVC}/aster_http_tls.c"
    "${SVC}/aster_mqtt.c" "${SVC}/aster_mqtt_codec.c" "${SVC}/aster_ws.c" "${SVC}/aster_ws_codec.c"
    INCLUDE_DIRS "." REQUIRES aster_render aster_connectivity lwip esp_netif esp_hw_support freertos mbedtls)
target_compile_definitions(${COMPONENT_LIB} PUBLIC CONFIG_ASTER_NET=1 CONFIG_ASTER_HTTP=1 CONFIG_ASTER_MQTT=1 CONFIG_ASTER_WEBSOCKET=1 CONFIG_ASTER_HTTP_TLS=1)
