# SPDX-License-Identifier: MIT

set(WAMR_ROOT_DIR ${ASTER_ROOT}/modules/wamr CACHE PATH "WAMR root")
set(WAMR_BUILD_PLATFORM "zephyr")
# AOT codegen target follows the board profile's CPU arch, not a fixed SoC.
if(CONFIG_ASTER_CPU_ARCH_XTENSA)
  set(WAMR_BUILD_TARGET "XTENSA")
elseif(CONFIG_ASTER_CPU_ARCH_ARM)
  set(WAMR_BUILD_TARGET "THUMBV8")
elseif(CONFIG_ASTER_CPU_ARCH_RISCV)
  set(WAMR_BUILD_TARGET "RISCV64_LP64D")
elseif(CONFIG_ASTER_CPU_ARCH_X86)
  set(WAMR_BUILD_TARGET "X86_64")
endif()
# Physical devices execute AOT only. The independent POSIX test/simulator
# runtime enables its own WASM interpreter in systems/posix/CMakeLists.txt.
set(WAMR_BUILD_INTERP 0)
set(WAMR_BUILD_FAST_INTERP 0)
set(WAMR_BUILD_AOT 1)
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_GLOBAL_HEAP_POOL 0)
# Dual-bus (D/IRAM mirror) AOT text layout is Xtensa/ESP32-S3 specific; the
# board port selects it. AOT text writes via DRAM, executes via IRAM.
if(CONFIG_ASTER_CPU_ARCH_XTENSA)
  add_definitions(-DWASM_MEM_DUAL_BUS_MIRROR=1)
endif()

include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

set(WAMR_INC
  include
  ${WAMR_ROOT_DIR}/core/iwasm/include
  ${WAMR_ROOT_DIR}/core/iwasm/interpreter
  ${WAMR_ROOT_DIR}/core/iwasm/aot
  ${WAMR_ROOT_DIR}/core/iwasm/common
  ${WAMR_ROOT_DIR}/core/shared/platform/zephyr
  ${WAMR_ROOT_DIR}/core/shared/platform/include
  ${WAMR_ROOT_DIR}/core/shared/mem-alloc
  ${WAMR_ROOT_DIR}/core/shared/utils
)

target_sources(app PRIVATE
  aster_runtime.c
  aster_native.c
  ${WAMR_RUNTIME_LIB_SOURCE}
)

target_include_directories(app PRIVATE
  include
  ${WAMR_INC}
  ${CMAKE_CURRENT_SOURCE_DIR}/../core
  ${CMAKE_CURRENT_SOURCE_DIR}/../services
  ${ASTER_ROOT}/platform)

get_directory_property(WAMR_COMPILE_DEFS DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMPILE_DEFINITIONS)
target_compile_definitions(app PRIVATE ${WAMR_COMPILE_DEFS} BH_VPRINTF=vprintf)
target_compile_options(app PRIVATE -Wno-type-limits)
