# SPDX-License-Identifier: GPL-3.0-or-later
# LibRaw, pinned to the release the Python reference decodes with (rawpy 0.26
# bundles LibRaw 0.22.0), built as a static library from the upstream sources.
# LibRaw ships no CMake project of its own, so the target is defined here.
include(FetchContent)
set(SPECTRA_LIBRAW_VERSION 0.22.0)
set(SPECTRA_LIBRAW_COMMIT 0b56545a4f828743f28a4345cdfdd4c49f9f9a2a)
FetchContent_Declare(libraw_src
  GIT_REPOSITORY https://github.com/LibRaw/LibRaw.git
  GIT_TAG ${SPECTRA_LIBRAW_COMMIT}
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR spectra-no-cmake-here)  # sources only: do not look for a CMakeLists.txt
FetchContent_MakeAvailable(libraw_src)

file(GLOB_RECURSE LIBRAW_SOURCES ${libraw_src_SOURCE_DIR}/src/*.cpp)
# The *_ph.cpp files are placeholders for builds without the matching feature.
list(FILTER LIBRAW_SOURCES EXCLUDE REGEX "_ph\\.cpp$")
add_library(spectra_libraw STATIC ${LIBRAW_SOURCES})
target_include_directories(spectra_libraw SYSTEM PUBLIC ${libraw_src_SOURCE_DIR})
target_compile_definitions(spectra_libraw PUBLIC LIBRAW_NODLL PRIVATE USE_JPEG)
target_link_libraries(spectra_libraw PRIVATE JPEG::JPEG Threads::Threads)
find_package(ZLIB)
if(ZLIB_FOUND)
  target_compile_definitions(spectra_libraw PRIVATE USE_ZLIB)
  target_link_libraries(spectra_libraw PRIVATE ZLIB::ZLIB)
endif()
set_target_properties(spectra_libraw PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(spectra_libraw PRIVATE /W0 /wd4996)
  target_compile_definitions(spectra_libraw PRIVATE _CRT_NONSTDC_NO_DEPRECATE)
  target_link_libraries(spectra_libraw PRIVATE ws2_32)
else()
  target_compile_options(spectra_libraw PRIVATE -w)
endif()
