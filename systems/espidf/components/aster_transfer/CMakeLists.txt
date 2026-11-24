idf_build_get_property(ai_enabled ASTER_AI_ENABLED)
if(ai_enabled)
    set(ai_component aster_ai)
endif()
idf_component_register(SRCS "transfer.c" "serial.c" INCLUDE_DIRS "."
    REQUIRES aster_runtime aster_render console esp_driver_uart esp_driver_usb_serial_jtag
    PRIV_REQUIRES ${ai_component})
if(ai_enabled)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ASTER_AI_ENABLED=1)
endif()
