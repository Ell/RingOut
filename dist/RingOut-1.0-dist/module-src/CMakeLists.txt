cmake_minimum_required(VERSION 3.16)
project(recompcore_module_newabi C)

# New-ABI per-game module: head DolRecomp generated chunks + head DolRecomp's
# own cpu.c (self-consistent new CPUState: spr[1024] + mem2, no external_pointer)
# + the chassis ABI export glue.

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(GAME_ID "" CACHE STRING "Six-character disc ID")
set(GENERATED_DIR "" CACHE PATH "DolRecomp output directory")
set(DOLRECOMP_SRC "" CACHE PATH "DolRecomp src/ (provides cpu/cpu.h, common/types.h, cpu/cpu.c)")
set(GXRUNTIME_INC "" CACHE PATH "GXRuntime include/ (provides core/cpu.h for the chassis ABI header)")
set(CHASSIS_ABI_DIR "" CACHE PATH "chassis StaticRecomp dir (StaticRecompABI.h)")
set(MODULE_TEMPLATE "" CACHE PATH "module-template dir (module_export.c, gen_module_tables.py)")

set(MODULE_NAME "g${GAME_ID}_recomp")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MODULE_TABLES "${CMAKE_CURRENT_BINARY_DIR}/module_tables.inc")
add_custom_command(
  OUTPUT "${MODULE_TABLES}"
  COMMAND Python3::Interpreter "${MODULE_TEMPLATE}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol" "${MODULE_TABLES}"
  DEPENDS "${MODULE_TEMPLATE}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol"
  COMMENT "Generating module_tables.inc")

file(GLOB CHUNK_SOURCES "${GENERATED_DIR}/chunks/*.c")

add_library(${MODULE_NAME} SHARED
  "${MODULE_TEMPLATE}/module_export.c"
  "${MODULE_TABLES}"
  "${DOLRECOMP_SRC}/cpu/cpu.c"
  ${CHUNK_SOURCES})

set_target_properties(${MODULE_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "${MODULE_NAME}")
set_source_files_properties("${MODULE_TABLES}" PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)

target_compile_definitions(${MODULE_NAME} PRIVATE MODULE_GAME_ID="${GAME_ID}")

target_include_directories(${MODULE_NAME} PRIVATE
  "${GENERATED_DIR}"
  "${DOLRECOMP_SRC}"
  "${GXRUNTIME_INC}"
  "${CHASSIS_ABI_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(${MODULE_NAME} PRIVATE m)
# Hidden visibility + no-semantic-interposition turns the hot runtime helpers
# (mem_read/write*, ppc_fp_available, resolve_addr, psq*) from PLT calls into
# direct, LTO-inlinable calls. As a default-visibility .so these were bouncing
# through the PLT on every load/store/FP guard (88k mem_read32, 87k
# ppc_fp_available, 70k mem_write32 call sites) — the dominant FMV CPU cost.
# The two chassis entry points keep explicit visibility("default").
target_compile_options(${MODULE_NAME} PRIVATE
  -flto=thin -fvisibility=hidden -fvisibility-inlines-hidden -fno-semantic-interposition)
target_link_options(${MODULE_NAME} PRIVATE -flto=thin)
