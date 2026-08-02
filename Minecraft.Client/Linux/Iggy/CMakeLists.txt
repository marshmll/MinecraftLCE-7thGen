# Iggy on Linux: the vendor PS4 core, plus its own OpenGL renderer.
#
# See ../../../.claude/linux-port/IGGY.md for why this works at all. In short: the
# Iggy core exists in this tree only as prebuilt libraries, but the PS4 build is ELF
# x86-64 with the System V ABI, and Iggy's OpenGL GDraw backend ships as source. So
# the missing pieces are only a platform layer and ~250 lines of SDL glue.
#
# This directory builds:
#   mclinux_iggy   - the core + platform layer + GDraw-GL, for the client to link
#   iggy_spike     - a standalone harness that loads/renders SWFs without the game

# The top-level project() enables CXX only. This is the only directory in the port
# with C and assembly sources - LinuxRadShim.c and gdraw_sdl.c are C because the
# vendor .inl files they include are C, and linux_iggy_setjmp.S is hand-written
# assembly - so enable both here rather than widening the top-level project().
enable_language(C)
enable_language(ASM)

set(MCLINUX_IGGY_VENDOR_LIB
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Orbis/Iggy/lib/libiggy_orbis.a")
set(MCLINUX_IGGY_PATCHED_LIB "${CMAKE_CURRENT_BINARY_DIR}/libiggy_linux.a")
set(MCLINUX_IGGY_PATCH_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/patch_orbis_iggy.py")

if(NOT EXISTS "${MCLINUX_IGGY_VENDOR_LIB}")
    message(FATAL_ERROR
        "Iggy: ${MCLINUX_IGGY_VENDOR_LIB} is missing.\n"
        "This is the PS4 build of the Iggy core and is the only one in the tree that "
        "can be linked on Linux (the Windows .lib is an import library for a DLL, "
        "and the PS3/Vita ones are PowerPC/ARM).")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Rewrites the archive so GNU ld can consume it: drops the four Sony-specific
# objects, fixes the ELF OSABI, redirects setjmp/longjmp, and - critically - rewrites
# 2461 GOT-load relocations that ld would otherwise resolve as direct references.
add_custom_command(
    OUTPUT "${MCLINUX_IGGY_PATCHED_LIB}"
    COMMAND ${Python3_EXECUTABLE} "${MCLINUX_IGGY_PATCH_SCRIPT}"
            "${MCLINUX_IGGY_VENDOR_LIB}" "${MCLINUX_IGGY_PATCHED_LIB}"
            "${CMAKE_CURRENT_BINARY_DIR}/objs"
    DEPENDS "${MCLINUX_IGGY_VENDOR_LIB}" "${MCLINUX_IGGY_PATCH_SCRIPT}"
    COMMENT "Iggy: adapting libiggy_orbis.a for Linux"
    VERBATIM)

add_custom_target(mclinux_iggy_patched_lib DEPENDS "${MCLINUX_IGGY_PATCHED_LIB}")

# The platform layer and the GDraw OpenGL backend.
#
# OBJECT rather than STATIC on purpose. The dependency runs *backwards* here: the
# vendor archive needs rrMutexCreate/_Log/iggy_default_system_alloc/... from
# LinuxRadShim.c, not the other way round. In a static library our objects would be
# offered to the linker before anything referenced them, skipped, and then the vendor
# archive would fail with undefined references to every symbol this layer exists to
# provide. An object library is placed directly on the consumer's link line, so it is
# always present when the archive is scanned.
add_library(mclinux_iggy OBJECT
    LinuxRadShim.c
    linux_iggy_setjmp.S
    gdraw_sdl.c
    # Converts wide strings at the Iggy boundary: IggyUTF16 is 16-bit and Linux's
    # wchar_t is 32-bit. Must be in this object library, not the client, for the same
    # link-order reason - it defines the public Iggy names the vendor archive's
    # renamed symbols no longer provide.
    LinuxIggyUtf16.cpp
)
add_dependencies(mclinux_iggy mclinux_iggy_patched_lib)

target_include_directories(mclinux_iggy PUBLIC
    # iggy.h / gdraw.h / rrCore.h, and gdraw_wgl.h + the two gdraw_gl_*.inl files.
    # Reused from Windows64 rather than copied: they are the same across all five
    # platform directories, and the shared .inl has no Windows API dependency.
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Windows64/Iggy/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Windows64/Iggy/gdraw"
)

target_compile_definitions(mclinux_iggy PUBLIC _LINUX64)

target_link_libraries(mclinux_iggy PUBLIC
    "${MCLINUX_IGGY_PATCHED_LIB}"
    ${MCLINUX_SDL2_TARGET}
    OpenGL::GL
    m
    pthread
)

# gdraw_sdl.c compiles the vendor .inl files as-is; they predate several warnings we
# would otherwise have to litter the vendor source with pragmas to silence. Keep our
# own two files warned normally.
set_source_files_properties(gdraw_sdl.c PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-variable")

# Standalone harness. Not linked into the game; it is the regression check for this
# layer, and the fastest way to test a single SWF.
#
#   iggy_spike --media Minecraft.Client <scene>.swf ...          # parse + tick, headless
#   iggy_spike --media Minecraft.Client --render <scene>.swf out.png
add_executable(iggy_spike iggy_spike.c)
target_link_libraries(iggy_spike PRIVATE mclinux_iggy)
