cmake_minimum_required(VERSION 3.20)
project(WarzoneAudioApo LANGUAGES CXX)

# Igual que la app: un build sin optimizar no tiene sitio en un camino de
# tiempo real. Aquí importa aún más -- este código corre dentro de
# audiodg.exe, y si no termina a tiempo corta el audio de TODO el sistema.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Un APO es un componente de Windows; solo compila ahi.")
endif()

add_library(WarzoneAudioApo SHARED
    src/WarzoneApo.cpp
    src/DllMain.cpp
    src/ApoGuids.cpp
    WarzoneApo.def
)

target_include_directories(WarzoneAudioApo PRIVATE
    include
    # La cadena DSP se comparte tal cual con la app de escritorio: un solo
    # sitio donde afinar el sonido, dos productos que lo usan.
    ${CMAKE_CURRENT_SOURCE_DIR}/../audio-optimizer/include
)

target_compile_definitions(WarzoneAudioApo PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_compile_options(WarzoneAudioApo PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /arch:AVX2 /fp:fast>
)

# Enlace estático del CRT: el APO se carga dentro de audiodg.exe, un proceso
# protegido del sistema. Depender de los redistribuibles de VC ahí es una
# fuente clásica de fallos de carga -- mejor no depender de nada externo.
set_property(TARGET WarzoneAudioApo PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_link_libraries(WarzoneAudioApo PRIVATE
    ole32
    oleaut32
    shell32
    advapi32
)

# Sin manifiesto embebido: la documentación de Microsoft es explícita en que
# un manifiesto embebido activa APIs prohibidas dentro del entorno protegido
# de audio, y el APO deja de cargar en cuanto se restaura la protección.
set_target_properties(WarzoneAudioApo PROPERTIES
    LINK_FLAGS "/MANIFEST:NO"
    OUTPUT_NAME "WarzoneAudioApo"
)
