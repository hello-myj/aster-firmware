# Zephyr service implementations: kernel/filesystem/UI/runtime integration.
add_subdirectory(core)
add_subdirectory(services)
add_subdirectory(shell)
if(CONFIG_ASTER_LVGL)
  add_subdirectory(ui)
endif()
if(CONFIG_ASTER_WAMR)
  add_subdirectory(runtime)
endif()
target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/core
  ${CMAKE_CURRENT_SOURCE_DIR}/services
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/ui
  ${ASTER_ROOT}/platform
  ${ASTER_ABI_INC})
