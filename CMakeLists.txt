# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)

set(ASTER_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include(cmake/hardware.cmake)

set(EXTRA_ZEPHYR_MODULES ${EXTRA_ZEPHYR_MODULES}
	${CMAKE_CURRENT_SOURCE_DIR}/modules/gc0308)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aster_firmware C CXX)

# ASTER ABI headers are the single source of truth, shared with the SDK
set(ASTER_ABI_INC ${CMAKE_CURRENT_SOURCE_DIR}/contract/include)

add_subdirectory(systems/zephyr)
add_subdirectory(platform)
if(CONFIG_ASTER_IMGDEC)
  add_subdirectory(modules/imgdec)
endif()
