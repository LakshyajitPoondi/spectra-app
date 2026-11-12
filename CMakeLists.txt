# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(spectra-app VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SPECTRA_BUILD_TESTS "Build the test suite" ON)

# The reference pipeline relies on IEEE semantics (NaN propagation, exact
# rounding), so fast-math is never enabled.
if(MSVC)
  add_compile_options(/W4 /permissive- /fp:precise /utf-8 /wd4244 /wd4267 /wd4100)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -ffp-contract=off)
endif()

find_package(Threads REQUIRED)

add_library(spectra_third_party INTERFACE)
target_include_directories(spectra_third_party SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pocketfft)
target_compile_definitions(spectra_third_party INTERFACE POCKETFFT_NO_MULTITHREADING=0)

add_library(spectra_core STATIC libs/core/src/math.cpp libs/core/src/npy.cpp)
target_include_directories(spectra_core PUBLIC libs/core/include)
target_link_libraries(spectra_core PUBLIC Threads::Threads)

add_library(spectra_data STATIC libs/data/src/pack.cpp)
target_include_directories(spectra_data PUBLIC libs/data/include)
target_link_libraries(spectra_data PUBLIC spectra_core spectra_third_party)

add_library(spectra_model STATIC
  libs/model/src/colour.cpp
  libs/model/src/curves.cpp
  libs/model/src/filters.cpp
  libs/model/src/diffusion.cpp
  libs/model/src/couplers.cpp
  libs/model/src/grain.cpp
  libs/model/src/gamut.cpp
  libs/model/src/upsampling.cpp
  libs/model/src/metering.cpp)
target_include_directories(spectra_model PUBLIC libs/model/include)
target_link_libraries(spectra_model PUBLIC spectra_data)

add_library(spectra_pipeline STATIC
  libs/pipeline/src/params_json.cpp
  libs/pipeline/src/digest.cpp
  libs/pipeline/src/pipeline.cpp)
target_include_directories(spectra_pipeline PUBLIC libs/pipeline/include)
target_link_libraries(spectra_pipeline PUBLIC spectra_model)

# Image and RAW file I/O. Needs LibRaw, libtiff, libpng, libjpeg and OpenEXR
# (vcpkg manifest on Windows, distribution packages on Linux). When they are
# missing the core libraries and the parity suite still build.
option(SPECTRA_WITH_IO "Build image / RAW file I/O" ON)
set(SPECTRA_HAVE_IO OFF)
if(SPECTRA_WITH_IO)
  find_package(TIFF)
  find_package(PNG)
  find_package(JPEG)
  find_package(OpenEXR CONFIG)
  find_package(libraw CONFIG QUIET)
  if(NOT TARGET libraw::raw_r AND NOT TARGET libraw::raw)
    find_path(LIBRAW_INCLUDE_DIR libraw/libraw.h)
    find_library(LIBRAW_LIBRARY NAMES raw_r raw)
  endif()
  if(TIFF_FOUND AND PNG_FOUND AND JPEG_FOUND AND OpenEXR_FOUND AND
     (TARGET libraw::raw_r OR TARGET libraw::raw OR (LIBRAW_INCLUDE_DIR AND LIBRAW_LIBRARY)))
    set(SPECTRA_HAVE_IO ON)
    add_library(spectra_io STATIC libs/io/src/image_io.cpp libs/io/src/raw.cpp)
    target_include_directories(spectra_io PUBLIC libs/io/include)
    target_link_libraries(spectra_io PUBLIC spectra_model PRIVATE TIFF::TIFF PNG::PNG JPEG::JPEG OpenEXR::OpenEXR)
    if(TARGET libraw::raw_r)
      target_link_libraries(spectra_io PRIVATE libraw::raw_r)
    elseif(TARGET libraw::raw)
      target_link_libraries(spectra_io PRIVATE libraw::raw)
    else()
      target_include_directories(spectra_io PRIVATE ${LIBRAW_INCLUDE_DIR})
      target_link_libraries(spectra_io PRIVATE ${LIBRAW_LIBRARY})
    endif()
  else()
    message(WARNING "Image / RAW I/O libraries not found: building without file I/O")
  endif()
endif()

add_executable(spectra-cli apps/spectra-cli/main.cpp apps/spectra-cli/parity.cpp)
target_link_libraries(spectra-cli PRIVATE spectra_pipeline)
if(SPECTRA_HAVE_IO)
  target_link_libraries(spectra-cli PRIVATE spectra_io)
  target_compile_definitions(spectra-cli PRIVATE SPECTRA_HAVE_IO=1)
endif()

if(SPECTRA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
