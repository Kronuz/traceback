# traceback: a small, modern C++ crash-traceback toolkit extracted from Xapiand.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
#
# Two layers over one .cc: the formatting box (capture/describe/format,
# symbolized via dladdr + abi::__cxa_demangle, macOS `atos` opt-in) and the
# whole-process crash dumper (thread registry, SIGUSR2 stack walk, all-thread
# snapshot, and an opt-in __cxa_throw/malloc interposition for throw-site stacks).

cmake_minimum_required(VERSION 3.16)
project(traceback CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# --- Build options: two independent opt-ins ---
# traceback.cc used Xapiand's project-wide "config.h" for one thing: the
# XAPIAND_TRACEBACKS gate. Standalone, the library owns the options itself, the
# way the times/allocators siblings own their feature macros.
#
# TRACEBACK_HOOKS (default OFF) gates the crash dumper: the fixed multi-megabyte
# lock-free thread registry, the SIGUSR2 stack-walk handler, and the all-thread
# dump_callstacks()/snapshot(). It does NOT touch the process allocator, so
# describe()/format() stay safe.
#
# TRACEBACK_THROW_HOOKS (default OFF) gates the throw-site tracker: the
# __cxa_throw + malloc/calloc/realloc/free interposition that records throw-site
# callstacks. It is split out from the dumper because that allocator override is
# a landmine (it fights tcmalloc/jemalloc and sanitizers and routes every free —
# foreign buffers included — through itself), so a host can take the dumper
# without it. The two compose; each is independent.
#
# The default build (both OFF) is just the lightweight formatting box, carrying
# no static registry, no signal handler, and no allocator override. The options
# are #cmakedefine'd into a generated traceback_config.h that traceback.h
# includes (guarded by __has_include, so a FetchContent-populate consumer that
# skips this CMake can -D them itself).
option(TRACEBACK_HOOKS "Build the crash dumper: thread registry + SIGUSR2 handler + all-thread dump (off by default)" OFF)
option(TRACEBACK_THROW_HOOKS "Build the throw-site tracker: __cxa_throw + malloc/free interposition (allocator landmine; off by default)" OFF)

# Registry sizing (only relevant under TRACEBACK_HOOKS). TRACEBACK_MAX_THREADS is
# the fixed registry array's size; TRACEBACK_MAX_FRAMES is each slot's per-thread
# callstack depth. Both are #cmakedefine'd into the generated config so a
# consumer can tune the registry's footprint without touching the source.
set(TRACEBACK_MAX_THREADS 1024 CACHE STRING "Max threads the crash-dumper registry can track (fixed array size; TRACEBACK_HOOKS only)")
set(TRACEBACK_MAX_FRAMES 128 CACHE STRING "Max frames stored per thread in the crash-dumper registry (TRACEBACK_HOOKS only)")
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/traceback_config.h.in"
	"${CMAKE_CURRENT_BINARY_DIR}/traceback_config.h"
	@ONLY
)

# --- Dependencies (the Kronuz family, tracked at tip) ---
# The dumper formats colored, per-thread tracebacks; these are the standalone
# siblings that supply what Xapiand pulled from its own tree:
#   strings     -> strings::format / strings::indent   (strings.hh)
#   errno-names -> error::name                          (error.hh)
#   term-color  -> the color palette (colors.h: STEEL_BLUE, DARK_ORANGE, ...)
#   nanosleep   -> nanosleep()                          (nanosleep.h)
# likely.h is vendored in-repo (4-line macro), like the compressors sibling.
FetchContent_Declare(
	strings
	GIT_REPOSITORY https://github.com/Kronuz/strings.git
	GIT_TAG        main
)
FetchContent_Declare(
	errno_names
	GIT_REPOSITORY https://github.com/Kronuz/errno-names.git
	GIT_TAG        main
)
FetchContent_Declare(
	term_color
	GIT_REPOSITORY https://github.com/Kronuz/term-color.git
	GIT_TAG        main
)
FetchContent_Declare(
	nanosleep
	GIT_REPOSITORY https://github.com/Kronuz/nanosleep.git
	GIT_TAG        main
)
FetchContent_MakeAvailable(strings errno_names term_color nanosleep)

# --- Library target ---
add_library(traceback traceback.cc)
target_include_directories(traceback PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_BINARY_DIR}"   # generated traceback_config.h
)
target_compile_features(traceback PUBLIC cxx_std_20)
add_library(traceback::traceback ALIAS traceback)

# strings/term_color/nanosleep expose namespaced ALIAS targets; errno-names'
# target is the plain `errno_names`. All carry their include dirs transitively,
# so traceback.cc finds strings.hh / error.hh / colors.h / nanosleep.h.
target_link_libraries(traceback PUBLIC
	strings::strings
	errno_names
	term_color::term_color
	nanosleep::nanosleep
)

# dladdr: libdl on Linux, in libSystem on macOS.
find_library(DL_LIB dl)
if(DL_LIB)
	target_link_libraries(traceback PUBLIC ${DL_LIB})
endif()

# forkpty (macOS atos path): libutil on Linux/BSD, in libSystem on macOS.
if(NOT APPLE)
	find_library(UTIL_LIB util)
	if(UTIL_LIB)
		target_link_libraries(traceback PUBLIC ${UTIL_LIB})
	endif()
endif()

# backtrace(3): some libcs (e.g. musl) put it in a separate libexecinfo.
find_library(EXECINFO_LIB execinfo)
if(EXECINFO_LIB)
	target_link_libraries(traceback PUBLIC ${EXECINFO_LIB})
endif()

# pthread: the dumper signals registered threads (pthread_kill) and walks
# pthread_self(). On most platforms this is Threads::Threads.
find_package(Threads)
if(Threads_FOUND)
	target_link_libraries(traceback PUBLIC Threads::Threads)
endif()

# Keep frame pointers so backtraces are reliable in optimized builds. The signal
# handler's in-place stack walk depends on a valid frame-pointer chain.
target_compile_options(traceback PRIVATE -fno-omit-frame-pointer)

# Only build the test when this repo is the top-level project, so consumers
# (e.g. via FetchContent) don't build it.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	enable_testing()
	add_executable(traceback_test test/test.cc)
	target_link_libraries(traceback_test PRIVATE traceback)
	add_test(NAME traceback COMMAND traceback_test)

	# A runnable tour of the library (not a test): build & run ./build/traceback_demo
	add_executable(traceback_demo examples/demo.cc)
	target_link_libraries(traceback_demo PRIVATE traceback)
endif()
