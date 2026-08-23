cmake_minimum_required(VERSION 3.13)
project(sc55d VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(NUKED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/nuked-sc55)
if(NOT EXISTS ${NUKED_DIR}/src/mcu.cpp)
    message(FATAL_ERROR
        "vendor/nuked-sc55 is empty. Run: git submodule update --init --recursive")
endif()

find_package(ALSA REQUIRED)
find_package(Threads REQUIRED)

# Emulation core, straight out of the submodule. Left out on purpose:
#   lcd.cpp          SDL2 video (see src/stubs.cpp)
#   midi_rtmidi.cpp  RtMidi input (see src/midi_in.cpp)
#   midi_win32.cpp   Windows only
set(CORE_SOURCES
    ${NUKED_DIR}/src/mcu.cpp
    ${NUKED_DIR}/src/mcu_interrupt.cpp
    ${NUKED_DIR}/src/mcu_opcodes.cpp
    ${NUKED_DIR}/src/mcu_timer.cpp
    ${NUKED_DIR}/src/pcm.cpp
    ${NUKED_DIR}/src/submcu.cpp
    ${NUKED_DIR}/src/utils/files.cpp)

# Upstream's SDL frontend lives in mcu.cpp rather than a file of its own, so it
# cannot simply be dropped from the build: main() sits in the same translation
# unit as the MCU itself. Rename it out of the way instead of patching the
# submodule. The renamed function is dead code; sc55d's own main() is in
# src/main.cpp, and the SDL calls left in that dead body resolve against the
# shim in src/sdl_compat.cpp.
set_source_files_properties(${NUKED_DIR}/src/mcu.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=nuked_sc55_sdl_main_unused")

set(SC55D_SOURCES
    src/audio_out.cpp
    src/bench.cpp
    src/core.cpp
    src/main.cpp
    src/midi_in.cpp
    src/romset.cpp
    src/rt.cpp
    src/sdl_compat.cpp
    src/stubs.cpp)

# Warnings on our code only: the core has its own idea of what is worth warning
# about and we do not patch it.
set_source_files_properties(${SC55D_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra")

add_executable(sc55d ${CORE_SOURCES} ${SC55D_SOURCES})

target_include_directories(sc55d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdl_compat
    ${NUKED_DIR}/src
    ${NUKED_DIR}/3rdparty/Utf8Main)

target_link_libraries(sc55d PRIVATE ALSA::ALSA Threads::Threads)

include(GNUInstallDirs)
install(TARGETS sc55d RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
