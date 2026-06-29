# traceback: a small, modern C++ stack-trace box extracted from Xapiand.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
#
# Header-driven (traceback.h) over one .cc. Symbolization uses dladdr +
# abi::__cxa_demangle; macOS `atos` (file:line) is opt-in via enable_atos().

cmake_minimum_required(VERSION 3.16)
project(traceback CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

add_library(traceback traceback.cc)
target_include_directories(traceback PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(traceback::traceback ALIAS traceback)

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

# Keep frame pointers so backtraces are reliable in optimized builds.
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
