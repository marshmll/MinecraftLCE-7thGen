# Minecraft.World's vcxproj has no per-file, per-platform exclusions and no
# extra include directories for the x64 configuration - every .cpp at this
# directory's root is compiled uniformly for every platform, with platform
# differences handled entirely by #ifdef inside the files. A directory glob
# therefore matches the vcxproj's file set exactly.
file(GLOB MCWORLD_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# SkyIslandDimension.cpp is orphaned: SkyIslandDimension.h does not exist
# anywhere in the repo (not just missing on Linux - it's simply absent from
# the leak), and nothing outside this one file references the class. Dead,
# unbuildable code predating this port; excluded rather than fabricating a
# header for a class nothing uses.
list(REMOVE_ITEM MCWORLD_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/SkyIslandDimension.cpp")

# ZonedChunkStorage.cpp is stale, not platform-specific: it references
# LevelChunk members (blocks/data/skyLight/blockLight/unsaved/fixBlocks) that
# don't exist on the current LevelChunk (which has m_unsaved and a sparse
# light-storage scheme instead) - a leftover from before some LevelChunk
# refactor, predating this port. It IS referenced from ZoneFile/ZoneIo/
# NbtSlotFile, so this is excluded rather than fixed blind (rewriting its
# ~15 call sites would mean guessing at a storage-format migration this
# port has no grounding for) - flagged here as a known pre-existing gap.
list(REMOVE_ITEM MCWORLD_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/ZonedChunkStorage.cpp")

list(APPEND MCWORLD_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/LinuxExtras/LinuxStubs.cpp")

add_library(Minecraft.World STATIC ${MCWORLD_SOURCES})

# x64headers mirrors the vcxproj's IncludePath override for Win32/x64/ARM64EC
# ($(ProjectDir)\x64headers;$(IncludePath)) - a handful of header shims
# (extraX64.h, qnet.h, xmcore.h, ...) that every desktop-class platform needs.
target_include_directories(Minecraft.World PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/x64headers
    ${CMAKE_CURRENT_SOURCE_DIR}/LinuxExtras
)

# Mirrors the Debug|x64 / Release|x64 PreprocessorDefinitions in
# Minecraft.World.vcxproj, with _WINDOWS64 replaced by _LINUX64 and the
# MSVC-CRT-only defines (_CRT_NON_CONFORMING_SWPRINTFS, _CRT_SECURE_NO_WARNINGS)
# dropped since they have no meaning for GCC/Clang.
target_compile_definitions(Minecraft.World PUBLIC
    _LARGE_WORLDS
    _DEBUG_MENUS_ENABLED
    _LIB
    _LINUX64
    $<$<CONFIG:Debug>:_DEBUG>
)

target_link_libraries(Minecraft.World PUBLIC pthread)

# ListTag<T>::print()/Tag::print() (NBT debug dumper, dead code - verified no
# caller anywhere in Minecraft.World or Minecraft.Client) has a pre-existing
# ostream/wostream mismatch that was never checked under MSVC's lazy
# uninstantiated-template handling. Modern GCC's -Wtemplate-body checks
# template bodies even without instantiation and flags it as an error; this
# restores the old lazy-checking behavior for that dead code rather than
# guessing at intended semantics for a print() nothing calls.
target_compile_options(Minecraft.World PRIVATE -Wno-template-body)

