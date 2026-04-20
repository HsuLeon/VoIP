cmake_minimum_required(VERSION 3.16)
project(speexdsp LANGUAGES C)

# Generate speexdsp_config_types.h for Windows x64
set(GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/include/speex)
file(MAKE_DIRECTORY ${GENERATED_INCLUDE_DIR})
file(WRITE ${GENERATED_INCLUDE_DIR}/speexdsp_config_types.h
"#ifndef __SPEEX_TYPES_H__\n"
"#define __SPEEX_TYPES_H__\n"
"#include <stdint.h>\n"
"typedef int16_t  spx_int16_t;\n"
"typedef uint16_t spx_uint16_t;\n"
"typedef int32_t  spx_int32_t;\n"
"typedef uint32_t spx_uint32_t;\n"
"#endif\n"
)

add_library(speexdsp STATIC
    libspeexdsp/buffer.c
    libspeexdsp/fftwrap.c
    libspeexdsp/filterbank.c
    libspeexdsp/jitter.c
    libspeexdsp/kiss_fft.c
    libspeexdsp/kiss_fftr.c
    libspeexdsp/mdf.c
    libspeexdsp/preprocess.c
    libspeexdsp/resample.c
    libspeexdsp/scal.c
    libspeexdsp/smallft.c
)

target_include_directories(speexdsp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libspeexdsp
        ${CMAKE_CURRENT_SOURCE_DIR}/win32
)

target_compile_definitions(speexdsp PRIVATE
    _CRT_SECURE_NO_WARNINGS
    HAVE_CONFIG_H
    FLOATING_POINT
    USE_SMALLFT
    EXPORT=
)

set_target_properties(speexdsp PROPERTIES
    C_STANDARD 99
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

install(TARGETS speexdsp ARCHIVE DESTINATION lib)
install(DIRECTORY include/speex DESTINATION include)
install(FILES ${GENERATED_INCLUDE_DIR}/speexdsp_config_types.h
        DESTINATION include/speex)
