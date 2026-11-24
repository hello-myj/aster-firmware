cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/../../../systems/espidf/ai.cmake")
list(APPEND EXTRA_COMPONENT_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/../../../modules/wamr/build-scripts/esp-idf"
  "${CMAKE_CURRENT_LIST_DIR}/../../../systems/espidf/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
aster_ai_configure_build()
set(WAMR_BUILD_THREAD_MGR 1 CACHE BOOL "Allow asynchronous guest termination" FORCE)
project(aster_osptek_p4_bringup)

if(ASTER_AI AND IDF_VERSION_MAJOR EQUAL 6 AND IDF_VERSION_MINOR EQUAL 0)
    # ESP-DL 3.3.13's optional JPEG frontend expects newer JPEG driver fields
    # than this board SDK exposes. WakeNet uses only the audio/DSP operators.
    idf_component_get_property(dl_lib espressif__esp-dl COMPONENT_LIB)
    get_target_property(dl_sources ${dl_lib} SOURCES)
    list(FILTER dl_sources EXCLUDE REGEX "(^|/)dl_image_jpeg[.]cpp$")
    set_property(TARGET ${dl_lib} PROPERTY SOURCES "${dl_sources}")
endif()

# P4 AOT validates PSRAM instruction mappings and synchronizes both core caches.
# Keep the vendor port intact for other boards, whose memory buses differ.
idf_component_get_property(WAMR_LIB wamr COMPONENT_LIB)
get_target_property(WAMR_SOURCES ${WAMR_LIB} SOURCES)
list(FILTER WAMR_SOURCES EXCLUDE REGEX "/espidf_memmap.c$")
set_property(TARGET ${WAMR_LIB} PROPERTY SOURCES "${WAMR_SOURCES}")
target_sources(${WAMR_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/port/wamr_memmap.c")
idf_component_get_property(ESP_MM_LIB esp_mm COMPONENT_LIB)
target_link_libraries(${WAMR_LIB} PRIVATE ${ESP_MM_LIB})
