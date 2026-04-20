cmake_minimum_required(VERSION 3.16)
project(rnnoise LANGUAGES C)

add_library(rnnoise STATIC
    src/celt_lpc.c
    src/denoise.c
    src/kiss_fft.c
    src/pitch.c
    src/rnn.c
    src/rnn_data.c
    src/rnn_reader.c
)

target_include_directories(rnnoise
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(rnnoise PRIVATE
    _CRT_SECURE_NO_WARNINGS
    _USE_MATH_DEFINES
)

set_target_properties(rnnoise PROPERTIES
    C_STANDARD 99
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

install(TARGETS rnnoise ARCHIVE DESTINATION lib)
install(FILES include/rnnoise.h DESTINATION include)
