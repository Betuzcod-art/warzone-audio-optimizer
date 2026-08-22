cmake_minimum_required(VERSION 3.20)
project(WarzoneAudioOptimizer LANGUAGES CXX)

# Esto es una app de audio en tiempo real: un build sin optimizar (Debug,
# el default de CMake si no se especifica nada) puede hacer que el hilo de
# render no termine de procesar un bloque dentro de su ventana de ~1-2ms y
# genere underruns audibles -- ya nos pasó. Si el usuario no pide Debug
# explícitamente, compilamos en Release.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

if(NOT WIN32)
    message(FATAL_ERROR "Este proyecto depende de WASAPI y solo compila en Windows.")
endif()

add_executable(WarzoneAudioOptimizer WIN32
    src/main.cpp
    src/WasapiLowLatencyEngine.cpp
)

target_include_directories(WarzoneAudioOptimizer PRIVATE
    include
)

target_compile_definitions(WarzoneAudioOptimizer PRIVATE
    NOMINMAX
)

# /arch:AVX2 habilita los intrínsecos usados en WarzoneAudioChain.h.
# Si el hardware objetivo no garantiza AVX2, retirar este flag: el código
# tiene fallback escalar automático vía #if defined(__AVX2__).
target_compile_options(WarzoneAudioOptimizer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /arch:AVX2 /fp:fast>
)

target_link_libraries(WarzoneAudioOptimizer PRIVATE
    avrt
    ole32
    winhttp
    shell32
)