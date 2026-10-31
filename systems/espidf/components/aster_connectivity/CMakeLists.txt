idf_component_register(SRCS "wifi.c" "wifi_service.c" "clock.c" "sdhost.c" INCLUDE_DIRS "." "../../../../contract/include" REQUIRES esp_wifi esp_driver_sdmmc esp_wifi_remote esp_hosted esp_event esp_netif nvs_flash freertos console esp_driver_uart)

# Apply the board's controller ownership functions only to Hosted's SDIO port.
# Registry sources remain intact and dependency checks keep working.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_get_property(HOSTED_LIB espressif__esp_hosted COMPONENT_LIB)
    idf_component_get_property(HOSTED_DIR espressif__esp_hosted COMPONENT_DIR)
    set_property(SOURCE "${HOSTED_DIR}/host/port/esp/freertos/src/port_esp_hosted_host_sdio.c"
        TARGET_DIRECTORY ${HOSTED_LIB} APPEND PROPERTY COMPILE_DEFINITIONS
        "sdmmc_host_init=aster_hosted_sdmmc_init"
        "sdmmc_host_deinit=aster_hosted_sdmmc_deinit")
endif()
