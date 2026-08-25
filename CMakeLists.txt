# SPDX-License-Identifier: MIT

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

# PGO wants a representative workload: --bench is exactly that. See
# docs/performance.md.
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
# Core patches
#
# Performance patches live in patches/ and are applied at build time to a copy
# of the core under the build tree. The submodule is never written to: it stays
# a pristine checkout of upstream, and every change we make is reviewable as a
# patch in this repository. This is also how Buildroot will want it.
#==============================================================================
# On by default. The patches are performance-only and validated bit-identical
# against 30 s of real SC-55mk2 audio (see patches/README.md). Romsets other
# than the mk2 family have not been checked -- if you run one, especially the
# JV-880, run scripts/validate-patches.sh first or configure with
# -DSC55D_PATCH_CORE=OFF.
option(SC55D_PATCH_CORE "Apply patches/ to a private copy of the core" ON)

# Upstream's development/decoder2 branch caches decoded instructions and skips
# MCU_Read for most memory loads. It is unreleased (due to be the default in
# core 0.7.0) and the submodule is pinned to master, so this is opt-in and needs
# a core checkout that actually carries the branch. Unvalidated here: no romset
# was available to render, so treat a decoder2 build as something to benchmark
# and digest-check, not something to deploy. See patches/README.md.
option(SC55D_DECODER2 "Build against upstream's unreleased decoder2 instruction decoder" OFF)

if(SC55D_DECODER2 AND NOT IS_DIRECTORY "${NUKED_DIR}/src/backend/decoder2")
    message(FATAL_ERROR
        "sc55d: -DSC55D_DECODER2=ON needs a core checkout carrying the decoder2 "
        "branch, and ${NUKED_DIR} has no src/backend/decoder2.\n"
        "  git -C vendor/nuked-sc55 fetch origin development/decoder2\n"
        "  git -C vendor/nuked-sc55 checkout FETCH_HEAD\n"
        "or point -DNUKED_DIR at a checkout of that branch.")
endif()

set(CORE_SRC_DIR "${NUKED_DIR}")

if(SC55D_PATCH_CORE)
    file(GLOB SC55D_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")
    list(SORT SC55D_PATCHES)

    if(SC55D_DECODER2)
        # 0003 fast-paths MCU_ReadCodeAdvance, which decoder2 takes off the hot
        # path entirely -- upstream calls it invalidated and they are right.
        # 0006 needs a rebase because decoder2 appends a member to mcu_t.
        list(FILTER SC55D_PATCHES EXCLUDE REGEX "/0003-mcu-code-fetch-fast-path\\.patch$")

        # Substituted in place, not appended: series order matters, and
        # patches/decoder2/0006 sorts after patches/0013 on full path.
        set(_sc55d_series "")
        foreach(_p IN LISTS SC55D_PATCHES)
            if(_p MATCHES "/0006-mcu-hot-field-layout\\.patch$")
                list(APPEND _sc55d_series
                    "${CMAKE_CURRENT_SOURCE_DIR}/patches/decoder2/0006-mcu-hot-field-layout.patch")
            else()
                list(APPEND _sc55d_series "${_p}")
            endif()
        endforeach()
        set(SC55D_PATCHES ${_sc55d_series})
    endif()

    if(SC55D_PATCHES)
        find_program(SC55D_PATCH_EXE NAMES patch)
        if(NOT SC55D_PATCH_EXE)
            message(FATAL_ERROR "patch(1) not found; install it or configure with -DSC55D_PATCH_CORE=OFF")
        endif()

        # Re-run configure when a patch changes.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SC55D_PATCHES})

        set(CORE_SRC_DIR "${CMAKE_BINARY_DIR}/core-patched")

        set(_sc55d_patch_hint "")
        if(NOT SC55D_DECODER2 AND IS_DIRECTORY "${NUKED_DIR}/src/backend/decoder2")
            set(_sc55d_patch_hint
                "This checkout carries the decoder2 branch; that series needs "
                "-DSC55D_DECODER2=ON.\n")
        endif()

        # Start from a clean copy so re-configuring never double-applies.
        file(REMOVE_RECURSE "${CORE_SRC_DIR}")
        file(MAKE_DIRECTORY "${CORE_SRC_DIR}")
        file(COPY "${NUKED_DIR}/src" DESTINATION "${CORE_SRC_DIR}")

        foreach(patch IN LISTS SC55D_PATCHES)
            get_filename_component(patch_name "${patch}" NAME)
            execute_process(
                COMMAND "${SC55D_PATCH_EXE}" -p1 --forward --input "${patch}"
                WORKING_DIRECTORY "${CORE_SRC_DIR}"
                RESULT_VARIABLE patch_result
                OUTPUT_VARIABLE patch_output
                ERROR_VARIABLE patch_output)
            if(NOT patch_result EQUAL 0)
                message(FATAL_ERROR
                    "sc55d: ${patch_name} does not apply to this core checkout.\n"
                    "${_sc55d_patch_hint}"
                    "The submodule has probably moved. Rebase the patch, or configure "
                    "with -DSC55D_PATCH_CORE=OFF to build the core unmodified.\n"
                    "${patch_output}")
            endif()
            message(STATUS "sc55d: applied ${patch_name}")
        endforeach()
    endif()
else()
    message(STATUS "sc55d: building the core unpatched (-DSC55D_PATCH_CORE=ON to apply patches/)")
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
    ${CORE_SRC_DIR}/src/backend/config.cpp
    ${CORE_SRC_DIR}/src/backend/diagnostics.cpp
    ${CORE_SRC_DIR}/src/backend/emu.cpp
    ${CORE_SRC_DIR}/src/backend/file_hashing.cpp
    ${CORE_SRC_DIR}/src/backend/file_io.cpp
    ${CORE_SRC_DIR}/src/backend/lcd.cpp
    ${CORE_SRC_DIR}/src/backend/mcu.cpp
    ${CORE_SRC_DIR}/src/backend/mcu_interrupt.cpp
    ${CORE_SRC_DIR}/src/backend/mcu_opcodes.cpp
    ${CORE_SRC_DIR}/src/backend/mcu_timer.cpp
    ${CORE_SRC_DIR}/src/backend/pcm.cpp
    ${CORE_SRC_DIR}/src/backend/rom.cpp
    ${CORE_SRC_DIR}/src/backend/rom_io.cpp
    ${CORE_SRC_DIR}/src/backend/standard_romsets.cpp
    ${CORE_SRC_DIR}/src/backend/submcu.cpp
    ${CORE_SRC_DIR}/src/backend/sha/sha224-256.c
    ${CORE_SRC_DIR}/src/common/gain.cpp
    ${CORE_SRC_DIR}/src/common/path_util.cpp
    ${CORE_SRC_DIR}/src/common/rom_loader.cpp)

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

if(SC55D_DECODER2)
    # Globbed rather than listed: the branch is unreleased and still moving, and
    # upstream's own CMakeLists just adds every .cpp in the directory.
    file(GLOB SC55D_DECODER2_SOURCES "${CORE_SRC_DIR}/src/backend/decoder2/*.cpp")
    list(SORT SC55D_DECODER2_SOURCES)
    if(NOT SC55D_DECODER2_SOURCES)
        message(FATAL_ERROR "sc55d: no sources found in ${CORE_SRC_DIR}/src/backend/decoder2")
    endif()
    list(APPEND CORE_SOURCES ${SC55D_DECODER2_SOURCES})
    set(NUKED_ENABLE_DECODER2 1)
    list(LENGTH SC55D_DECODER2_SOURCES _sc55d_decoder2_count)
    message(STATUS "sc55d: decoder2 enabled (${_sc55d_decoder2_count} extra core sources)")
endif()
configure_file(${CORE_SRC_DIR}/src/backend/config.h.in backend/config.h @ONLY)
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
    src/midi_queue.cpp
    src/ring.cpp
    src/rt.cpp)

# Warnings on our code only; the core has its own warning policy and we do not
# patch it.
set_source_files_properties(${SC55D_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra")

add_executable(sc55d ${CORE_SOURCES} ${SC55D_SOURCES})

target_include_directories(sc55d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CORE_SRC_DIR}/src/backend
    ${CORE_SRC_DIR}/src/common
    ${CMAKE_CURRENT_BINARY_DIR}/backend)

target_link_libraries(sc55d PRIVATE ALSA::ALSA Threads::Threads)

include(GNUInstallDirs)
install(TARGETS sc55d RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
