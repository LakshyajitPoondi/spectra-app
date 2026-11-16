# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(spectra-app VERSION 0.6.0 LANGUAGES CXX)

# The product name shown in the app, the installer and exported files. This is
# the only place it is defined.
set(SPECTRA_PRODUCT_NAME "Spectra (working title)")

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
if(WIN32)
  target_link_libraries(spectra_core PUBLIC psapi)
endif()

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
  libs/model/src/metering.cpp
  libs/model/src/extended.cpp)
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
  # LibRaw: by default the pinned upstream release is fetched and built
  # (cmake/libraw.cmake). -DSPECTRA_SYSTEM_LIBRAW=ON uses an installed one.
  option(SPECTRA_SYSTEM_LIBRAW "Use the system LibRaw instead of the pinned release" OFF)
  if(TIFF_FOUND AND PNG_FOUND AND JPEG_FOUND AND OpenEXR_FOUND)
    if(SPECTRA_SYSTEM_LIBRAW)
      find_path(LIBRAW_INCLUDE_DIR libraw/libraw.h REQUIRED)
      find_library(LIBRAW_LIBRARY NAMES raw_r raw REQUIRED)
      add_library(spectra_libraw INTERFACE)
      target_include_directories(spectra_libraw INTERFACE ${LIBRAW_INCLUDE_DIR})
      target_link_libraries(spectra_libraw INTERFACE ${LIBRAW_LIBRARY})
    else()
      include(cmake/libraw.cmake)
    endif()
    set(SPECTRA_HAVE_IO ON)
    add_library(spectra_io STATIC libs/io/src/image_io.cpp libs/io/src/raw.cpp)
    target_include_directories(spectra_io PUBLIC libs/io/include)
    target_link_libraries(spectra_io PUBLIC spectra_model
      PRIVATE TIFF::TIFF PNG::PNG JPEG::JPEG OpenEXR::OpenEXR spectra_libraw)
  else()
    message(WARNING "Image / RAW I/O libraries not found: building without file I/O")
  endif()
endif()

# Vulkan compute backend. The SPIR-V is committed (shaders/generated/spirv.cpp)
# so building needs only the Vulkan headers and loader; when glslc is available
# the file is regenerated from shaders/*.comp as part of the build.
option(SPECTRA_WITH_GPU "Build the Vulkan compute backend" ON)
set(SPECTRA_HAVE_GPU OFF)
if(SPECTRA_WITH_GPU)
  find_package(Vulkan)
  if(Vulkan_FOUND)
    set(SPECTRA_HAVE_GPU ON)
    set(SPECTRA_KERNELS pointwise blur iir resample reduce expose develop grain spectral color encode)
    set(SPIRV_CPP ${CMAKE_CURRENT_SOURCE_DIR}/shaders/generated/spirv.cpp)
    find_program(GLSLC glslc HINTS ENV VULKAN_SDK PATH_SUFFIXES Bin bin)
    if(GLSLC)
      set(spv_files)
      foreach(k ${SPECTRA_KERNELS})
        set(spv ${CMAKE_CURRENT_BINARY_DIR}/spv/${k}.spv)
        add_custom_command(OUTPUT ${spv}
          COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/spv
          COMMAND ${GLSLC} -O --target-env=vulkan1.1 -I ${CMAKE_CURRENT_SOURCE_DIR}/shaders
                  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${k}.comp -o ${spv}
          DEPENDS shaders/${k}.comp shaders/common.glsl VERBATIM)
        list(APPEND spv_files ${spv})
      endforeach()
      add_custom_command(OUTPUT ${SPIRV_CPP}
        COMMAND ${CMAKE_COMMAND} -DOUT=${SPIRV_CPP} -DSPV_DIR=${CMAKE_CURRENT_BINARY_DIR}/spv
                "-DNAMES=${SPECTRA_KERNELS}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
        DEPENDS ${spv_files} cmake/embed_spirv.cmake VERBATIM)
    endif()
    add_library(spectra_gpu STATIC libs/gpu/src/context.cpp libs/gpu/src/gpu_pipeline.cpp ${SPIRV_CPP})
    target_include_directories(spectra_gpu PUBLIC libs/gpu/include)
    target_link_libraries(spectra_gpu PUBLIC spectra_pipeline Vulkan::Vulkan)
    if(NOT MSVC)
      target_compile_options(spectra_gpu PRIVATE -Wno-missing-field-initializers)
    endif()
  else()
    message(WARNING "Vulkan headers / loader not found: building without the GPU backend")
  endif()
endif()

# Rendering engine shared by the app and the CLI (no UI dependency).
configure_file(libs/engine/include/spectra/engine/product.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/spectra/engine/product.hpp @ONLY)
add_library(spectra_engine STATIC
  libs/engine/src/engine.cpp libs/engine/src/presets.cpp libs/engine/src/lut.cpp libs/engine/src/files.cpp)
target_include_directories(spectra_engine PUBLIC libs/engine/include ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(spectra_engine PUBLIC spectra_pipeline)
if(SPECTRA_HAVE_GPU)
  target_link_libraries(spectra_engine PUBLIC spectra_gpu)
  target_compile_definitions(spectra_engine PUBLIC SPECTRA_HAVE_GPU=1)
endif()
if(SPECTRA_HAVE_IO)
  target_link_libraries(spectra_engine PUBLIC spectra_io)
  target_compile_definitions(spectra_engine PUBLIC SPECTRA_HAVE_IO=1)
endif()

add_executable(spectra-cli apps/spectra-cli/main.cpp apps/spectra-cli/parity.cpp apps/spectra-cli/bench.cpp
               apps/spectra-cli/tools.cpp)
target_link_libraries(spectra-cli PRIVATE spectra_engine)


# The desktop application (Qt 6 Widgets). Skipped when Qt is not installed, so
# the libraries, the CLI and the tests build without it.
option(SPECTRA_BUILD_APP "Build the Qt desktop application" ON)
set(SPECTRA_HAVE_APP OFF)
if(SPECTRA_BUILD_APP)
  find_package(Qt6 6.4 QUIET COMPONENTS Core Gui Widgets)
  if(Qt6_FOUND)
    set(SPECTRA_HAVE_APP ON)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    add_executable(spectra-app WIN32
      apps/spectra-app/main.cpp
      apps/spectra-app/main_window.cpp apps/spectra-app/main_window.hpp
      apps/spectra-app/param_panel.cpp apps/spectra-app/param_panel.hpp
      apps/spectra-app/preview_widget.cpp apps/spectra-app/preview_widget.hpp
      apps/spectra-app/render_controller.cpp apps/spectra-app/render_controller.hpp
      apps/spectra-app/resources/resources.qrc)
    target_link_libraries(spectra-app PRIVATE spectra_engine Qt6::Core Qt6::Gui Qt6::Widgets)
    target_compile_definitions(spectra-app PRIVATE SPECTRA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
      QT_NO_KEYWORDS QT_DISABLE_DEPRECATED_BEFORE=0x060400)
    set(CMAKE_AUTOMOC OFF)
    set(CMAKE_AUTORCC OFF)
  else()
    message(WARNING "Qt 6 (Core, Gui, Widgets) not found: building without the desktop application")
  endif()
endif()

if(SPECTRA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
