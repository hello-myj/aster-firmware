include(${CMAKE_CURRENT_LIST_DIR}/../../../../common/sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../../common/setup/service.cmake)
idf_component_register(SRCS ${ASTER_COMMON_PACKAGE_SOURCES} "runtime.c" "aot_profile.c" "graphics.c" "files.c" "timers.c" "apps.c" "wifi.c" "clock.c" "network.c" "audio.c" "images.c" "devices.c" "../../../zephyr/services/aster_pkg.c"
 "../../../zephyr/services/aster_app_mgr.c" "../../../zephyr/services/aster_versions.c"
 "../../../zephyr/services/aster_storage.c" "../../../zephyr/services/aster_sig.c" "../../../zephyr/services/aster_kv.c" INCLUDE_DIRS ${ASTER_COMMON_INCLUDE_DIRS} "." "../../../../contract/include" REQUIRES aster_render aster_connectivity aster_transport aster_audio wamr esp_timer heap freertos)


target_sources(${COMPONENT_LIB} PRIVATE ${ASTER_SETUP_SERVICE_SOURCES})
add_dependencies(${COMPONENT_LIB} aster_setup_page)
target_compile_definitions(${COMPONENT_LIB} PRIVATE WASM_ENABLE_THREAD_MGR=1 ASTER_SYSTEM_ESP_IDF=1 "CONFIG_ASTER_TARGET_ID=\"riscv\"" CONFIG_ASTER_AOT_TARGET_ID=2)
if(CONFIG_IDF_TARGET_ESP32P4)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ASTER_REQUIRE_AOT=1)
endif()
if(CONFIG_WAMR_ENABLE_AOT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_ASTER_AOT=1)
endif()
