# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: cmake -DOUT=<file.cpp> -DSPV_DIR=<dir> -DNAMES="a;b;c" -P embed_spirv.cmake
# Writes one C++ file holding every SPIR-V module as a uint32_t array plus the
# lookup function spectra::gpu::find_shader().
set(code "// SPDX-License-Identifier: GPL-3.0-or-later\n// Generated from shaders/*.comp by cmake/embed_spirv.cmake. Do not edit.\n")
string(APPEND code "#include \"spectra/gpu/context.hpp\"\n\nnamespace spectra::gpu {\nnamespace {\n")
foreach(name ${NAMES})
  file(READ "${SPV_DIR}/${name}.spv" hex HEX)
  string(LENGTH "${hex}" len)
  math(EXPR words "${len} / 8")
  # SPIR-V is little-endian: bytes b0 b1 b2 b3 form the word 0xb3b2b1b0.
  string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1," body "${hex}")
  string(REGEX REPLACE "(0x........,0x........,0x........,0x........,0x........,0x........,0x........,0x........,)" "\\1\n" body "${body}")
  string(APPEND code "const uint32_t k_${name}[] = {\n${body}};\n")
endforeach()
string(APPEND code "}  // namespace\n\nShaderBlob find_shader(const std::string& name) {\n")
foreach(name ${NAMES})
  string(APPEND code "    if (name == \"${name}\") return {k_${name}, sizeof(k_${name}) / sizeof(uint32_t)};\n")
endforeach()
string(APPEND code "    return {nullptr, 0};\n}\n\n}  // namespace spectra::gpu\n")
file(WRITE "${OUT}" "${code}")
