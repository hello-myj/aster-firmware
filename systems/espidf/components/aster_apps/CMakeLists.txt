idf_component_register(SRCS "apps.c" INCLUDE_DIRS "." REQUIRES aster_runtime aster_render aster_ui freertos esp_timer)
if(CONFIG_IDF_TARGET_ESP32P4)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ASTER_REQUIRE_AOT=1)
endif()
