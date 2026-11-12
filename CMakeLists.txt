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

add_executable(spectra-cli apps/spectra-cli/main.cpp apps/spectra-cli/parity.cpp)
target_link_libraries(spectra-cli PRIVATE spectra_pipeline)

if(SPECTRA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
