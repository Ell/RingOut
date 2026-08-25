# Cross-compile Ring Out for 64-bit Windows from a Linux host.
#
# The compiler names can be overridden before including this file, which lets
# CI use either distro MinGW-w64 GCC or an llvm-mingw toolchain on PATH.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc CACHE FILEPATH "MinGW C compiler")
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++ CACHE FILEPATH "MinGW C++ compiler")
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres CACHE FILEPATH "MinGW resource compiler")

# Windows' filesystem is case-insensitive, and Dolphin contains system-header
# includes with the SDK's display casing (for example <SetupAPI.h>).  MinGW's
# Linux sysroot stores those headers lowercase, so provide tiny case aliases.
set(_ringout_mingw_case_headers
    "${CMAKE_CURRENT_LIST_DIR}/mingw-case-headers")
set(CMAKE_C_FLAGS_INIT "-I${_ringout_mingw_case_headers}")
set(CMAKE_CXX_FLAGS_INIT "-I${_ringout_mingw_case_headers}")

execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -print-sysroot
    OUTPUT_VARIABLE _ringout_mingw_sysroot
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT _ringout_mingw_sysroot)
    # Distro MinGW-w64 GCC commonly reports an empty sysroot while installing
    # its target headers and libraries in this conventional prefix.
    set(_ringout_mingw_sysroot "/usr/x86_64-w64-mingw32")
endif()
set(CMAKE_FIND_ROOT_PATH "${_ringout_mingw_sysroot}")

# Programs such as Python and Git run on the Linux build host.  Headers,
# libraries, and CMake packages must come only from the Windows target root.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config does not obey CMAKE_FIND_ROOT_PATH on its own.  Without these
# settings it can inject /usr/include and Linux libraries into Windows compile
# commands, producing conflicts between glibc and the UCRT declarations.
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${_ringout_mingw_sysroot}/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_ringout_mingw_sysroot}")
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF CACHE BOOL "" FORCE)
