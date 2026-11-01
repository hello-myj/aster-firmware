idf_component_register(SRCS "transfer.c" "serial.c" INCLUDE_DIRS "."
    REQUIRES aster_runtime aster_render console esp_driver_uart)
