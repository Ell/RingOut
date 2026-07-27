cmake_minimum_required(VERSION 3.16)
project(recompcore_module C)

# Per-game native module for RecompCore: generated chunks + DolRecomp CPU
# runtime + ABI export glue, no other host dependencies.
#
# Configure with:
#   cmake -B build -DGAME_ID=G4QE01 \
#         -DGENERATED_DIR=/path/to/generated \
#         -DGXRUNTIME_DIR=/path/to/GXRuntime

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(GAME_ID "" CACHE STRING "Six-character disc ID, e.g. G4QE01")
if(NOT GAME_ID)
  message(FATAL_ERROR "Set -DGAME_ID=<disc id> (e.g. -DGAME_ID=G4QE01)")
endif()
set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH "DolRecomp output directory (generated.h, generated_smc.txt, main.dol, chunks/)")
if(NOT EXISTS "${GENERATED_DIR}")
  message(FATAL_ERROR "Generated directory not found at ${GENERATED_DIR}. Set -DGENERATED_DIR=<path to DolRecomp output>")
endif()
set(GXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../GXRuntime" CACHE PATH "GXRuntime checkout")
if(NOT EXISTS "${GXRUNTIME_DIR}")
  message(FATAL_ERROR "GXRuntime not found at ${GXRUNTIME_DIR}. Set -DGXRUNTIME_DIR=<path to GXRuntime>")
endif()
set(CHASSIS_ABI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Source/Core/Core/PowerPC/StaticRecomp"
    CACHE PATH "RecompCore StaticRecomp ABI directory")

set(MODULE_NAME "g${GAME_ID}_recomp")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MODULE_TABLES "${CMAKE_CURRENT_BINARY_DIR}/module_tables.inc")
add_custom_command(
  OUTPUT "${MODULE_TABLES}"
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol" "${MODULE_TABLES}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol"
  COMMENT "Generating module_tables.inc")

file(GLOB CHUNK_SOURCES "${GENERATED_DIR}/chunks/*.c")

add_library(${MODULE_NAME} SHARED
  module_export.c
  "${MODULE_TABLES}"
  "${GXRUNTIME_DIR}/src/core/cpu.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter_table.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter_float.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter_integer.c"
  ${CHUNK_SOURCES})

set_target_properties(${MODULE_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "${MODULE_NAME}" UNITY_BUILD OFF)
set_source_files_properties("${MODULE_TABLES}" PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)

target_compile_definitions(${MODULE_NAME} PRIVATE MODULE_GAME_ID="${GAME_ID}")

target_include_directories(${MODULE_NAME} PRIVATE
  "${GENERATED_DIR}"
  "${GXRUNTIME_DIR}/include"
  "${CHASSIS_ABI_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")

if(NOT WIN32)
  target_link_libraries(${MODULE_NAME} PRIVATE m)
endif()

# Keep each generated chunk as its own translation unit. Clang uses ThinLTO and
# lld; GCC deliberately avoids LTO until the differential parity gate covers it.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  target_compile_options(${MODULE_NAME} PRIVATE -O2 -flto=thin -fvisibility=hidden
                         -ffp-contract=off -fno-fast-math)
  target_link_options(${MODULE_NAME} PRIVATE -flto=thin)
  if(UNIX AND NOT APPLE)
    target_link_options(${MODULE_NAME} PRIVATE -fuse-ld=lld)
  endif()
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(${MODULE_NAME} PRIVATE -O2 -fvisibility=hidden
                         -ffp-contract=off -fno-fast-math)
elseif(MSVC)
  target_compile_options(${MODULE_NAME} PRIVATE /fp:strict)
endif()
