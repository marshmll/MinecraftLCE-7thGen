cmake_minimum_required(VERSION 3.16)
project(MinecraftLCE_Linux LANGUAGES CXX)

# This CMake build is an additive, independent build path whose only current
# target is Linux x64. It does not replace the .sln/.vcxproj files, which
# remain the build path for Windows and the 7th/8th-gen consoles.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This CMake build currently only supports Linux. "
                         "Use MinecraftPC.sln / MinecraftConsoles.sln for Windows and console builds.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This CMake build currently only supports 64-bit (x86_64) Linux.")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# GNU extensions predefine a `linux` macro (=1), which mangles
# Minecraft.Client/Minecraft.h's `enum OS { linux, ... }`. Strict ISO mode
# doesn't define it.
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_subdirectory(Minecraft.World)
add_subdirectory(Minecraft.Client/Linux)
