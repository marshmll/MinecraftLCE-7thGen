# Miles Sound System on Linux: the vendor PS4 build, relinked.
#
# Same manoeuvre as ../Iggy, on the other RAD Game Tools middleware in this tree.
# Minecraft.Client/Orbis/Miles/lib/*.a is ELF x86-64 with the System V ABI, so the
# core links directly; patch_orbis_miles.py fixes the packaging and LinuxSceShim.c
# supplies the Sony OS calls underneath.
#
# Audio is an easier case than Iggy: no setjmp anywhere in these archives, and no
# object has to be dropped - every Sony call site is behind a symbol we can provide,
# so the vendor's own threading, timing, file and audio-driver code is all kept.
#
# This directory builds:
#   mclinux_miles - the Miles core + Bink Audio + the sce* layer, for the client
#   miles_spike   - a standalone harness that loads a soundbank and plays an event

# The top-level project() enables CXX only; LinuxSceShim.c is C.
enable_language(C)

set(MCLINUX_MILES_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Orbis/Miles")
set(MCLINUX_MILES_PATCH_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/patch_orbis_miles.py")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# mssorbis.a  - the core: mixer, streams, the event system and the .msscmp loader.
# binkaorbis.a - Bink Audio, the codec behind every .binka music file. It is
#                registered by SoundEngine.cpp's Register_RIB(BinkADec), which needs
#                the static-RIB build of mss.h - see LinuxMiles.h.
#
# Not linked: fltorbis.a (the DSP filter set - unused by SoundEngine.cpp, and it has
# an unresolved ChorusMain) and mssorbismidi.a (MIDI/DLS - the game ships no MIDI).
set(MCLINUX_MILES_ARCHIVES mssorbis binkaorbis)
set(MCLINUX_MILES_PATCHED_LIBS "")

foreach(archive IN LISTS MCLINUX_MILES_ARCHIVES)
    set(vendor_lib "${MCLINUX_MILES_VENDOR_DIR}/lib/${archive}.a")
    set(patched_lib "${CMAKE_CURRENT_BINARY_DIR}/lib${archive}_linux.a")

    if(NOT EXISTS "${vendor_lib}")
        message(FATAL_ERROR
            "Miles: ${vendor_lib} is missing.\n"
            "This is the PS4 build and is the only one in the tree that can be linked "
            "on Linux (Windows/Durango ship MSVC .lib files, and the PS3/Vita ones are "
            "PowerPC/ARM).")
    endif()

    add_custom_command(
        OUTPUT "${patched_lib}"
        COMMAND ${Python3_EXECUTABLE} "${MCLINUX_MILES_PATCH_SCRIPT}"
                "${vendor_lib}" "${patched_lib}"
                "${CMAKE_CURRENT_BINARY_DIR}/objs/${archive}"
        DEPENDS "${vendor_lib}" "${MCLINUX_MILES_PATCH_SCRIPT}"
        COMMENT "Miles: adapting ${archive}.a for Linux"
        VERBATIM)

    list(APPEND MCLINUX_MILES_PATCHED_LIBS "${patched_lib}")
endforeach()

add_custom_target(mclinux_miles_patched_libs DEPENDS ${MCLINUX_MILES_PATCHED_LIBS})

# OBJECT rather than STATIC, for the same reason as mclinux_iggy: the dependency runs
# backwards. The vendor archives need scePthreadMutexInit/sceAudioOutOutput/... from
# LinuxSceShim.c, so our objects have to be on the link line unconditionally rather
# than offered from an archive nothing has referenced yet.
add_library(mclinux_miles OBJECT
    LinuxSceShim.c
)
add_dependencies(mclinux_miles mclinux_miles_patched_libs)

target_include_directories(mclinux_miles PUBLIC
    # For LinuxMiles.h's consumers. mss.h itself is reached through that header, never
    # directly, so the PS4 ABI selection cannot be bypassed by accident.
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MCLINUX_MILES_VENDOR_DIR}/include"
)

target_compile_definitions(mclinux_miles PUBLIC _LINUX64)

target_link_libraries(mclinux_miles PUBLIC
    ${MCLINUX_MILES_PATCHED_LIBS}
    ${MCLINUX_SDL2_TARGET}
    m
    pthread
)

# Standalone harness. Not linked into the game; it is the regression check for this
# layer and the fastest way to confirm the blob is alive without booting Minecraft.
#
#   miles_spike Durango/Sound/Minecraft.msscmp            # list events
#   miles_spike Durango/Sound/Minecraft.msscmp <event>    # play one
add_executable(miles_spike miles_spike.c)
target_link_libraries(miles_spike PRIVATE mclinux_miles)
