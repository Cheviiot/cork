# CMake toolchain file for cross compiling to Windows with cork.
#
#   eval "$(cork env)"
#   cmake -B build -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE="$CORK_TOOLCHAIN_FILE" \
#         -DCORK_TARGET=x64
#
# CORK_TARGET selects the architecture: x64 (default), x86 or arm64. The
# wrappers for it must be on PATH, which `cork env --arch <arch>` arranges.
#
# Dependencies built for Windows go on CMAKE_FIND_ROOT_PATH; this file appends
# the toolchain to whatever is already there rather than replacing it.

if(NOT DEFINED CORK_TARGET)
    set(CORK_TARGET "x64")
endif()

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)

# CMAKE_SYSTEM_PROCESSOR takes the names Windows itself uses, not the ones the
# wrappers are called by: CMake code in the wild tests it against AMD64 and x86.
if(CORK_TARGET STREQUAL "x64")
    set(CMAKE_SYSTEM_PROCESSOR AMD64)
elseif(CORK_TARGET STREQUAL "x86")
    set(CMAKE_SYSTEM_PROCESSOR x86)
elseif(CORK_TARGET STREQUAL "arm64")
    set(CMAKE_SYSTEM_PROCESSOR ARM64)
else()
    message(FATAL_ERROR "cork: unknown CORK_TARGET '${CORK_TARGET}'; use x64, x86 or arm64")
endif()

# The tools are named without the .exe suffix on purpose: those are the native
# wrappers, and going through them is the whole point. Naming the Windows
# binary here would hand CMake something it cannot execute at all.
find_program(CMAKE_C_COMPILER   NAMES cl   REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES cl   REQUIRED)
find_program(CMAKE_RC_COMPILER  NAMES rc   REQUIRED)
find_program(CMAKE_LINKER       NAMES link REQUIRED)
find_program(CMAKE_AR           NAMES lib  REQUIRED)
find_program(CMAKE_MT           NAMES mt   REQUIRED)

# CMake decides a compiler is MSVC by compiling a probe and reading the
# predefined macros, which works through the wrappers. What it cannot do is
# link and run that probe, because the result is a Windows binary: so the
# check is told to stop at the static library.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Look for libraries and headers inside the toolchain, never on the Linux host.
# Without this, find_library cheerfully hands a Windows build /usr/lib/libz.so
# and the mistake only surfaces at link time, or later.
#
# The toolchain's own root comes from where this file sits, so it is right for
# whichever generation is in use. Anything appended by the caller is kept:
# dependency prefixes belong on this list, and replacing it would be the one
# way to make finding anything impossible.
get_filename_component(CORK_GENERATION_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
list(APPEND CMAKE_FIND_ROOT_PATH "${CORK_GENERATION_ROOT}")

# Programs are the exception: the compiler, the linker and ninja are native
# Linux binaries and live on the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
