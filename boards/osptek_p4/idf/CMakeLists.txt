cmake_minimum_required(VERSION 3.20)
list(APPEND EXTRA_COMPONENT_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/../../../modules/wamr/build-scripts/esp-idf"
  "${CMAKE_CURRENT_LIST_DIR}/../../../systems/espidf/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(WAMR_BUILD_THREAD_MGR 1 CACHE BOOL "Allow asynchronous guest termination" FORCE)
project(aster_osptek_p4_bringup)

# P4 AOT validates PSRAM instruction mappings and synchronizes both core caches.
# Keep the vendor port intact for other boards, whose memory buses differ.
idf_component_get_property(WAMR_LIB wamr COMPONENT_LIB)
get_target_property(WAMR_SOURCES ${WAMR_LIB} SOURCES)
list(FILTER WAMR_SOURCES EXCLUDE REGEX "/espidf_memmap.c$")
set_property(TARGET ${WAMR_LIB} PROPERTY SOURCES "${WAMR_SOURCES}")
target_sources(${WAMR_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/port/wamr_memmap.c")
idf_component_get_property(ESP_MM_LIB esp_mm COMPONENT_LIB)
target_link_libraries(${WAMR_LIB} PRIVATE ${ESP_MM_LIB})
