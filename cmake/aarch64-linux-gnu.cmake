# Cross-compile sc55d for 64-bit ARM (Raspberry Pi 3 and later).
#
#   sudo dpkg --add-architecture arm64        # plus an arm64 apt source
#   sudo apt install g++-aarch64-linux-gnu libasound2-dev:arm64
#   cmake -S . -B build-arm64 -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
#         -DSC55D_CPU=cortex-a53
#
# Native builds on the Pi itself do not need this; it is for CI and for
# checking ARM code generation from a development machine.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Multiarch puts the arm64 libraries alongside the host's, so search both.
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
