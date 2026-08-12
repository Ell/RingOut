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

# ThinLTO codegen runs inside the LINKER, not the compiler, so the linker and
# its cache belong here rather than with the compile flags below.
#
# Measured on this tree, 12 cores, clang 22.1.8, 132 chunks, generated sources
# reused so only the module build is timed:
#
#   clean, default linker (ld.bfd + LLVMgold)          3m 18s
#   clean, lld, no cache                               3m 04s
#   clean, lld, cold cache                     2m 59s / 3m 24s
#   FRESH BUILD DIR, lld, WARM cache                   2m 17s
#   one chunk changed, lld, no cache                     77.5s
#   one chunk changed, default linker, no cache          80.6s
#   one chunk changed, lld, warm cache             1.4s / 1.7s
#   nothing changed                                     0.013s
#
# THE LINKER'S EFFECT ON A CLEAN BUILD IS NOT RESOLVED, and the two figures on
# the cold-cache line are why: that is one configuration measured twice, 25s
# apart, a spread wider than the 14s between the two linkers. Anything claimed
# from a single pair of clean builds on this machine -- including a "33% faster
# clean build" -- is reading thermals and page cache. lld is enabled here
# because the CACHE needs it (--thinlto-cache-dir is an lld option; the gold
# plugin spells it -plugin-opt=cache-dir= and was never measured), not because
# a clean build got measurably faster.
#
# The cache is the actual win, and it is not subtle. A clean build is 132
# parallel clang invocations, so no linker swap can reach that part -- but what
# the cache removes is ThinLTO codegen for every module that did not change,
# which is the whole 77 seconds of the link. Note the fresh-build-dir line: even
# with a warm cache a new build tree still pays all 132 compiles, so the cache
# halves that case rather than collapsing it. ccache would cover the other half.
#
# The 1.4s is a lower bound, honestly labelled: the test edit was dead-stripped,
# so every module still cache-hit and only the chunk recompile (~1.2s) and a
# fully-cached link (~0.15s) were measured. A chunk whose code really changes
# adds codegen for that one module, which the 77s/132 rate puts near 0.6s. Call
# a real one-chunk edit ~2s rather than ~1.4s. Either way it is ~40x, not the
# 420x that comparing against a no-op build would suggest.
#
# setup.sh does `rm -rf work/` every run, so this only pays off on a re-run --
# which setup.sh explicitly invites ("worth re-running after you have played",
# to pick up the memory-card icon). That re-run currently rebuilds a module that
# is byte-for-byte what it already had.
option(MODULE_LLD "Link with lld when it is usable; ThinLTO links far faster" ON)
set(MODULE_LINK_CACHE "" CACHE STRING
    "ThinLTO cache directory; empty picks the XDG default, NONE disables it")

# PROBED, never assumed. lld is a separate package from clang on Arch, Debian
# and Fedora alike, and setup.sh takes whatever compiler it finds (clang -> gcc
# -> cc). This file already has one scar from assuming a clang spelling was
# present -- see the -flto=thin note above, which killed the module build on
# every machine without it. A player who has clang but not lld must get the
# ordinary linker, not a failed setup.
if(MODULE_LTO AND MODULE_LLD AND CMAKE_C_COMPILER_ID MATCHES "Clang" AND UNIX AND NOT APPLE)
  include(CheckCSourceCompiles)
  set(CMAKE_REQUIRED_LINK_OPTIONS -fuse-ld=lld)
  check_c_source_compiles("int main(void){return 0;}" MODULE_LLD_USABLE)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
endif()

if(MODULE_LLD AND MODULE_LLD_USABLE)
  target_link_options(${MODULE_NAME} PRIVATE -fuse-ld=lld)

  # NOT under module-src/. package-dist.sh copies this whole tree into the
  # release stage with `cp -a`, so a cache next to these sources would ship 42 MB
  # of one developer's build artefacts inside the zip, and would sit untracked in
  # git besides. It has to outlive `rm -rf work/`, which is the one thing work/
  # cannot do, so it goes to the user's XDG cache.
  if(NOT MODULE_LINK_CACHE)
    if(DEFINED ENV{XDG_CACHE_HOME})
      set(MODULE_LINK_CACHE "$ENV{XDG_CACHE_HOME}/ringout/thinlto")
    elseif(DEFINED ENV{HOME})
      set(MODULE_LINK_CACHE "$ENV{HOME}/.cache/ringout/thinlto")
    endif()
  endif()

  # Bounded on purpose: one clean build leaves 135 entries / 42 MB, and every
  # rebuilt variant adds more. lld's default policy prunes on a 20-minute
  # interval and will otherwise let the directory grow to half the free space on
  # the disk, which is not a reasonable thing to leave on a player's machine.
  if(MODULE_LINK_CACHE AND NOT MODULE_LINK_CACHE STREQUAL "NONE")
    file(MAKE_DIRECTORY "${MODULE_LINK_CACHE}")
    target_link_options(${MODULE_NAME} PRIVATE
      "-Wl,--thinlto-cache-dir=${MODULE_LINK_CACHE}"
      "-Wl,--thinlto-cache-policy=cache_size_bytes=2g:prune_after=168h")
    message(STATUS "module: ThinLTO cache at ${MODULE_LINK_CACHE}")
  endif()
elseif(MODULE_LLD AND CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(STATUS "module: lld not usable, linking with the default linker")
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
#
# It covers the LINKER too, which had to be checked rather than assumed. Because
# ThinLTO codegen happens inside the linker, an lld-linked module's .text differs
# from the ld.bfd one -- same 20,689,042 bytes, different contents -- and
# "different code that should behave identically" is exactly what FRAME 176 above
# disproved once. Measured 2026-08-12: two modules built from identical sources,
# one per linker, give byte-identical per-frame RAM hashes over 24000 frames of
# arcade-match.txt (boot -> Arcade -> two full matches with active input), the
# same standard the compiler-version and -march comparisons were held to.
# -DMODULE_LLD=OFF restores the old linker if a future parity run points here.
#
# Identical guest state says nothing about how many CYCLES it takes to reach it,
# so that was measured separately (module-bench.sh, 24000 frames, 7 runs per
# arm): ld.bfd 979.4 Gcyc mean, lld 994.1, i.e. +1.5% for lld -- inside a 12-16%
# per-arm spread on this host, and one whole session came out 997.8 vs 997.4, a
# dead heat. Instruction counts agree to 0.03% and all 14 runs hash identically,
# so both arms really did the same work. NO in-game cost is detectable.
#
# An earlier reading of "lld is 8% slower" came from three adjacent-run pairs in
# a single session and did not survive replication: across both sessions the
# seven pairs run -7.8% to +8.8% with the sign flipping. This host cannot resolve
# anything under ~10% on this workload, so a real codegen comparison needs a
# quiet machine or the Deck. Do not re-derive that 8% and act on it.
target_compile_options(${MODULE_NAME} PRIVATE
  ${MODULE_LTO_FLAG} -fvisibility=hidden -fno-semantic-interposition
  -ffp-contract=off)
target_link_options(${MODULE_NAME} PRIVATE ${MODULE_LTO_FLAG})
