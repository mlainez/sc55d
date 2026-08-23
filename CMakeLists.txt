cmake_minimum_required(VERSION 3.20)
project(sc55d VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)          # the emulation core requires C++23
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Normally the submodule. Overridable so a checkout elsewhere can be built
# against without touching the submodule.
set(NUKED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/nuked-sc55" CACHE PATH
    "Path to the Nuked-SC55 checkout (jcmoyer fork)")

if(NOT EXISTS ${NUKED_DIR}/src/backend/emu.cpp)
    message(FATAL_ERROR
        "${NUKED_DIR} does not look like the jcmoyer Nuked-SC55 fork. "
        "Run: git submodule update --init --recursive")
endif()

#==============================================================================
# Performance options
#
# The core is an interpreter running a cycle-level model of two CPUs and a PCM
# chip; it is entirely bound by how well this loop compiles. These matter more
# than anything in sc55d's own code, especially on a Pi 3.
#==============================================================================
include(CheckCXXCompilerFlag)
include(CheckIPOSupported)

option(SC55D_LTO "Link-time optimization (inlines the core across its translation units)" ON)

set(SC55D_CPU "" CACHE STRING
    "Target CPU for -mcpu/-mtune: cortex-a72 (Pi 4), cortex-a53 (Pi 3), native, or empty")

set(SC55D_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, or use")
set_property(CACHE SC55D_PGO PROPERTY STRINGS off generate use)

if(SC55D_LTO)
    check_ipo_supported(RESULT SC55D_HAVE_IPO OUTPUT SC55D_IPO_ERROR)
    if(SC55D_HAVE_IPO)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        message(STATUS "sc55d: link-time optimization enabled")
    else()
        message(WARNING "sc55d: LTO requested but unsupported: ${SC55D_IPO_ERROR}")
    endif()
endif()

if(SC55D_CPU)
    check_cxx_compiler_flag("-mcpu=${SC55D_CPU}" SC55D_HAVE_MCPU)
    if(SC55D_HAVE_MCPU)
        add_compile_options(-mcpu=${SC55D_CPU})
        message(STATUS "sc55d: building for -mcpu=${SC55D_CPU}")
    else()
        # x86 and some toolchains spell it -march/-mtune instead.
        check_cxx_compiler_flag("-mtune=${SC55D_CPU}" SC55D_HAVE_MTUNE)
        if(SC55D_HAVE_MTUNE)
            add_compile_options(-mtune=${SC55D_CPU})
            message(STATUS "sc55d: building for -mtune=${SC55D_CPU}")
        else()
            message(WARNING "sc55d: the compiler rejects ${SC55D_CPU}; ignoring SC55D_CPU")
        endif()
    endif()
endif()

# PGO wants a representative workload: --bench is exactly that. See README.
set(SC55D_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "Where PGO profile data lives")
if(SC55D_PGO STREQUAL "generate")
    add_compile_options(-fprofile-generate=${SC55D_PGO_DIR})
    add_link_options(-fprofile-generate=${SC55D_PGO_DIR})
    message(STATUS "sc55d: building an instrumented binary; run it with --bench, then rebuild with -DSC55D_PGO=use")
elseif(SC55D_PGO STREQUAL "use")
    add_compile_options(-fprofile-use=${SC55D_PGO_DIR} -fprofile-correction -Wno-missing-profile)
    add_link_options(-fprofile-use=${SC55D_PGO_DIR})
    message(STATUS "sc55d: building with profile data from ${SC55D_PGO_DIR}")
elseif(NOT SC55D_PGO STREQUAL "off")
    message(FATAL_ERROR "SC55D_PGO must be off, generate, or use")
endif()

#==============================================================================
# Dependencies
#==============================================================================
find_package(ALSA REQUIRED)
find_package(Threads REQUIRED)

#==============================================================================
# Emulation core (jcmoyer fork), compiled straight out of the submodule
#
# Only the backend library and the frontend-shared ROM loader are built. The
# fork's own CMakeLists is not used: it hard-requires RtMidi on Linux even when
# its SDL frontend is disabled, and building the core as part of this target
# lets LTO and PGO cover the emulation loop as well as sc55d.
#==============================================================================
set(CORE_SOURCES
    ${NUKED_DIR}/src/backend/config.cpp
    ${NUKED_DIR}/src/backend/diagnostics.cpp
    ${NUKED_DIR}/src/backend/emu.cpp
    ${NUKED_DIR}/src/backend/file_hashing.cpp
    ${NUKED_DIR}/src/backend/file_io.cpp
    ${NUKED_DIR}/src/backend/lcd.cpp
    ${NUKED_DIR}/src/backend/mcu.cpp
    ${NUKED_DIR}/src/backend/mcu_interrupt.cpp
    ${NUKED_DIR}/src/backend/mcu_opcodes.cpp
    ${NUKED_DIR}/src/backend/mcu_timer.cpp
    ${NUKED_DIR}/src/backend/pcm.cpp
    ${NUKED_DIR}/src/backend/rom.cpp
    ${NUKED_DIR}/src/backend/rom_io.cpp
    ${NUKED_DIR}/src/backend/standard_romsets.cpp
    ${NUKED_DIR}/src/backend/submcu.cpp
    ${NUKED_DIR}/src/backend/sha/sha224-256.c
    ${NUKED_DIR}/src/common/gain.cpp
    ${NUKED_DIR}/src/common/path_util.cpp
    ${NUKED_DIR}/src/common/rom_loader.cpp)

# The core's config.h is generated from a template that wants the core's own
# version, so read it out of the fork's CMakeLists instead of reporting ours.
set(_sc55d_our_version_major ${CMAKE_PROJECT_VERSION_MAJOR})
set(_sc55d_our_version_minor ${CMAKE_PROJECT_VERSION_MINOR})
set(_sc55d_our_version_patch ${CMAKE_PROJECT_VERSION_PATCH})
set(_sc55d_our_version ${CMAKE_PROJECT_VERSION})

file(STRINGS "${NUKED_DIR}/CMakeLists.txt" _nuked_project REGEX "project *\\(nuked-sc55 VERSION")
if(_nuked_project MATCHES "VERSION ([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(CMAKE_PROJECT_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(CMAKE_PROJECT_VERSION_MINOR ${CMAKE_MATCH_2})
    set(CMAKE_PROJECT_VERSION_PATCH ${CMAKE_MATCH_3})
    set(CMAKE_PROJECT_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
endif()
set(NUKED_ENABLE_ASIO 0)
set(NUKED_SOURCE "sc55d")
configure_file(${NUKED_DIR}/src/backend/config.h.in backend/config.h @ONLY)
set(NUKED_CORE_VERSION ${CMAKE_PROJECT_VERSION})

set(CMAKE_PROJECT_VERSION_MAJOR ${_sc55d_our_version_major})
set(CMAKE_PROJECT_VERSION_MINOR ${_sc55d_our_version_minor})
set(CMAKE_PROJECT_VERSION_PATCH ${_sc55d_our_version_patch})
set(CMAKE_PROJECT_VERSION ${_sc55d_our_version})

message(STATUS "sc55d: emulation core version ${NUKED_CORE_VERSION} from ${NUKED_DIR}")

#==============================================================================
# sc55d
#==============================================================================
set(SC55D_SOURCES
    src/audio_out.cpp
    src/bench.cpp
    src/core.cpp
    src/main.cpp
    src/midi_in.cpp
    src/rt.cpp)

# Warnings on our code only; the core has its own warning policy and we do not
# patch it.
set_source_files_properties(${SC55D_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra")

add_executable(sc55d ${CORE_SOURCES} ${SC55D_SOURCES})

target_include_directories(sc55d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${NUKED_DIR}/src/backend
    ${NUKED_DIR}/src/common
    ${CMAKE_CURRENT_BINARY_DIR}/backend)

target_link_libraries(sc55d PRIVATE ALSA::ALSA Threads::Threads)

include(GNUInstallDirs)
install(TARGETS sc55d RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
