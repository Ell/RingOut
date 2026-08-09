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

# The C backend writes chunks as .c to compile; the LLVM backend writes them as
# .o to link. CMake accepts object files directly in a target's sources, so the
# only difference is which glob finds anything -- and an LLVM module additionally
# needs cpu_llvm_abi.c, because that backend CALLS the float helpers this fork's
# C backend emits inline.
file(GLOB CHUNK_SOURCES "${GENERATED_DIR}/chunks/*.c")
file(GLOB CHUNK_OBJECTS "${GENERATED_DIR}/chunks/*.o")

set(BACKEND_EXTRA_SOURCES "")
if(CHUNK_OBJECTS AND NOT CHUNK_SOURCES)
  set(BACKEND_EXTRA_SOURCES "${DOLRECOMP_SRC}/cpu/cpu_llvm_abi.c")
  set_source_files_properties(${CHUNK_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  message(STATUS "module: linking ${GENERATED_DIR}/chunks as LLVM objects")
endif()

add_library(${MODULE_NAME} SHARED
  "${MODULE_TEMPLATE}/module_export.c"
  "${MODULE_TABLES}"
  "${DOLRECOMP_SRC}/cpu/cpu.c"
  ${BACKEND_EXTRA_SOURCES}
  ${CHUNK_SOURCES}
  ${CHUNK_OBJECTS})

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
# -flto=thin is Clang's spelling; GCC only understands plain -flto and errors
# out on "thin". setup.sh probes clang -> gcc -> cc, so a machine without clang
# hit "unrecognized argument to '-flto=' option: 'thin'" and the module build
# died. -fvisibility-inlines-hidden is also C++-only and warns on every C file.
#
# Plain `-flto` also makes GCC run the whole back end serially: it reports
# "using serial compilation of 128 LTRANS jobs" and pins one core while the
# other eleven idle. Measured on a 12-core host, a relink of this module went
# from 22 minutes to 6 with `-flto=auto`, which hands LTRANS the job server /
# core count instead. setup.sh builds this on the user's machine, so that is
# most of a coffee break per setup run.
#
# Version-guarded rather than assumed, for exactly the reason the paragraph
# above records: `auto` only exists from GCC 10 (2020), and setup.sh takes
# whatever compiler it finds.
# -DMODULE_LTO=OFF drops LTO entirely. Needed for PGO instrumentation: the
# instrumented objects are LTO IR, and mixing them with a non-LTO link silently
# produces a 21 KB stub that loads and does nothing. Not for shipping -- LTO is
# worth 5-6 minutes of link time here and was measured, not assumed.
option(MODULE_LTO "Link the module with LTO" ON)
if(NOT MODULE_LTO)
  set(MODULE_LTO_FLAG -fno-lto)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
  set(MODULE_LTO_FLAG -flto=thin)
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 10)
  set(MODULE_LTO_FLAG -flto=auto)
else()
  set(MODULE_LTO_FLAG -flto)
endif()
# Line-level profiling (-DMODULE_DEBUG_LINES=ON). Each chunk is ONE huge C
# function, so perf can only ever report "func_8000D940" -- a 16 KB span holding
# dozens of guest functions. That is enough to see which chunk is hot and no more.
#
# -g1 emits line tables only, so `perf report -s srcline` resolves samples to a
# line of the generated .c; the emitter writes one `label_<guest addr>:` per guest
# instruction, so that line maps back to a guest PC.
# .github/scripts/hot-guest-code.sh does the mapping and prints hot guest
# addresses.
#
# Off by default: it roughly doubles the .so and there is no reason to ship it.
# Codegen is unaffected -- the .text bytes are identical with and without it --
# so a -g1 build stays timing-comparable with a normal one.
option(MODULE_DEBUG_LINES "Emit line tables so perf can attribute inside chunks" OFF)
if(MODULE_DEBUG_LINES)
  target_compile_options(${MODULE_NAME} PRIVATE -g1)
  target_link_options(${MODULE_NAME} PRIVATE -g1)
endif()

# -ffp-contract=off is NOT a tuning choice, it is what makes this module
# reproducible across machines, and netplay depends on that.
#
# GCC defaults to -ffp-contract=fast, so it may fuse an incidental a*b+c in the
# emitted C into an FMA -- and whether it does depends on -march. Measured: a
# module built -march=x86-64-v3 and one built -march=znver2 diverge in guest RAM
# at FRAME 176. setup.sh builds with -march=native, so two desktop players on
# different CPUs get modules that cannot stay in sync, and a Deck cannot play a
# PC. With this flag the same two builds are byte-identical over 600 frames.
#
# It costs nothing measurable (7.20s vs 7.33s on a 600-frame run, i.e. inside
# the noise and if anything faster): the guest's own FMA instructions are
# emitted as explicit ppc_fma() calls, so auto-contraction was only ever
# catching incidental arithmetic.
target_compile_options(${MODULE_NAME} PRIVATE
  ${MODULE_LTO_FLAG} -fvisibility=hidden -fno-semantic-interposition
  -ffp-contract=off)
target_link_options(${MODULE_NAME} PRIVATE ${MODULE_LTO_FLAG})
