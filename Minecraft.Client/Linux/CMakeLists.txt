# Prefer a target-based find_package (what modern distros ship); fall back
# to the plain-variable form, then to pkg-config, so this keeps working on
# older SDL2 installs that only provide one of those.
find_package(SDL2 QUIET)

if(TARGET SDL2::SDL2)
    set(MCLINUX_SDL2_TARGET SDL2::SDL2)
elseif(SDL2_FOUND)
    add_library(MCLinux::SDL2 INTERFACE IMPORTED)
    target_include_directories(MCLinux::SDL2 INTERFACE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(MCLinux::SDL2 INTERFACE ${SDL2_LIBRARIES})
    set(MCLINUX_SDL2_TARGET MCLinux::SDL2)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(MCLINUX_SDL2_TARGET PkgConfig::SDL2)
endif()

find_package(OpenGL REQUIRED)

# GLEW is the GL 3.3 core function loader (glewInit() after context
# creation) - chosen over vendoring GLAD since glew-devel is a normal distro
# package (unlike GLAD, which needs its web generator or vendoring sources).
find_package(GLEW QUIET)
if(NOT TARGET GLEW::GLEW)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLEW REQUIRED IMPORTED_TARGET glew)
    add_library(GLEW::GLEW ALIAS PkgConfig::GLEW)
endif()

# Phase 7 trunk: essentially all of Minecraft.Client except the Iggy-backed
# modern UI. Undefined-symbol discovery (iterating add-file/rebuild, same
# process Minecraft.World went through in Phase 2) showed the gameplay/
# rendering/server/DLC/tutorial/gamerules clusters are all interdependent
# enough that a hand-picked file list doesn't converge - glob whole
# directories instead, mirroring Minecraft.World/CMakeLists.txt's own
# approach. Common/UI is the ONE directory NOT globbed: only its base
# infrastructure (UIController/UIGroup/UILayer/UIScene) plus a handful of
# non-Iggy "component" overlays are included; the ~100 concrete
# UIScene_*/IUIScene_*-menu-screen files are Iggy-only and out of scope (see
# UILayer.cpp's _LINUX64 guards on its two scene factories, and
# LinuxUIController.h).
file(GLOB MCLINUX_ROOT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../*.cpp")

# CreativeMode.cpp: not referenced anywhere in Minecraft.Client.vcxproj at
# all (not even excluded-for-x64 - simply absent), and calls Tile::use()/
# ItemInstance::useOn() overloads that don't exist on the current interface.
# Same category as Phase 2's SkyIslandDimension.cpp - dead/orphaned code
# predating this port, not a Linux-specific gap.
list(REMOVE_ITEM MCLINUX_ROOT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../CreativeMode.cpp")

# DemoLevel.cpp: zero external references anywhere (never instantiated), and
# its constructor calls a `Level(shared_ptr<LevelStorage>, wstring, __int64)`
# overload that doesn't exist on the current Level interface (only
# LevelSettings*-taking overloads do) - it only "compiled" on MSVC because a
# static-const-__int64-zero constant is loosely accepted as a null pointer
# constant there. Genuinely dead/broken code predating this port, not a
# Linux-specific gap - same category as SkyIslandDimension.cpp (Phase 2).
list(REMOVE_ITEM MCLINUX_ROOT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../DemoLevel.cpp")

# SurvivalMode.cpp WAS excluded here (calls to Tile::use()/ItemInstance::
# mineBlock() had drifted from their current signatures) - fixed directly in
# SurvivalMode.cpp (mineBlock now passes minecraft->level as the leading
# param it was missing; use()'s new clickedFace/clickX/Y/Z params are filled
# with `face` + 0.5f/0.5f/0.5f face-centre defaults, since useItemOn's own
# signature has no real click-position data to forward - a documented
# gameplay judgment call, not a mechanical fix). Re-included: it's load-
# bearing (Common/Tutorial/TutorialMode.cpp does `new SurvivalMode(this)`
# unconditionally - survival is the default game mode).

# The dead-Iggy-menu legacy XUI screens still left excluded (their real
# gameplay siblings - Screen/TitleScreen/ChatScreen/DeathScreen/ErrorScreen/
# InBedChatScreen - are load-bearing and stay in via the glob above). Every
# OTHER legacy-screen-named file turned out to be load-bearing too, once
# pulled back in one at a time via undefined-symbol discovery (TitleScreen/
# DeathScreen/etc construct them directly) - only these 8 (the actual
# in-game menu screens: inventory/crafting/furnace/container/pause/stats/
# text-edit/trap) are genuinely Iggy-menu-system-adjacent dead weight.
list(REMOVE_ITEM MCLINUX_ROOT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../AbstractContainerScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../AchievementScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../ContainerScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../CraftingScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../DisconnectedScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../FurnaceScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../InventoryScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../NameEntryScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../PauseScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../ReceivingLevelScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../StatsScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../TextEditScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../TrapScreen.cpp"
)

file(GLOB MCLINUX_COMMON_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../Common/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Audio/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Network/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Common/GameRules/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Tutorial/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Trial/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Colours/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Common/DLC/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Telemetry/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Common/zlib/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Leaderboards/*.cpp"
)

# NetworkPlayerXbox.cpp: despite the Xbox-sounding path, it's genuinely
# compiled by Minecraft.Client.vcxproj for Debug|x64/Release|x64 too (no
# per-config exclusion listed) - referenced by GameNetworkManager.cpp.
list(APPEND MCLINUX_COMMON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../Xbox/Network/NetworkPlayerXbox.cpp")

# Common/UI is now globbed like every other directory. It used to be the one
# hand-picked list in this file, because Iggy was believed to be unavailable on Linux
# and the ~100 concrete UIScene_*/UIControl_* menu screens are Iggy-backed. Iggy now
# builds and runs here (see .claude/linux-port/IGGY.md and the Iggy/ subdirectory), so
# the whole scene tree is in scope and UILayer.cpp's two scene factories are no longer
# guarded out.
file(GLOB MCLINUX_UI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../Common/UI/*.cpp")

# These are exactly the two Common/UI files Minecraft.Client.vcxproj excludes from its
# x64 configurations, so excluding them here follows the vcxproj rather than working
# around them:
#
#   UIControl_Touch.cpp - PSVita touch input. Everything it calls
#       (UIScene::GetParentLayer(), UIController::TouchBoxAdd()) is declared inside
#       `#ifdef __PSVITA__`.
#   UIScene_InGameSaveManagementMenu.cpp - console save-management screen. Uses
#       Sony's sceCesUcs* string conversion, a C4JStorage::ContinueIncompleteOperation()
#       that no shared header declares, and IDS_* strings absent from Windows64's
#       strings.h.
foreach(_ui_excluded UIControl_Touch.cpp UIScene_InGameSaveManagementMenu.cpp)
    list(REMOVE_ITEM MCLINUX_UI_SOURCES
         "${CMAKE_CURRENT_SOURCE_DIR}/../Common/UI/${_ui_excluded}")
endforeach()

# The ~56 per-entity *Renderer.cpp / ~34 *Model.cpp / ~29 *Particle.cpp files
# are already covered by MCLINUX_ROOT_SOURCES's glob above.

# Iggy: the vendor PS4 core adapted for Linux, plus Iggy's own OpenGL GDraw backend
# built from the source that ships in Windows64/Iggy/gdraw. This replaces the premise
# that Iggy is unavailable on Linux; see .claude/linux-port/IGGY.md.
#
# The client links this, so LinuxIggyShim.cpp's no-op stubs are gone from the source
# list below - they would now be duplicate definitions of the real entry points.
add_subdirectory(Iggy)

# Miles Sound System: the same manoeuvre on the other RAD middleware in this tree.
# The client links the vendor PS4 core, so LinuxAudioShim.{h,cpp} - the OpenAL
# reimplementation of the AIL_* API written in Phase 5, whose soundbank layer could
# only ever be a stub - is deleted, for the same reason LinuxIggyShim.cpp was: it
# would be a duplicate definition of every real entry point. OpenAL is no longer a
# dependency of this port; Miles reaches the device through SDL2.
add_subdirectory(Miles)

add_executable(Minecraft.Client.Linux
    Linux_App.cpp
    Linux_Minecraft.cpp
    LinuxExtras/LinuxRender.cpp
    LinuxExtras/LinuxInput.cpp
    LinuxExtras/LinuxStorage.cpp
    LinuxExtras/LinuxUIController.cpp
    LinuxExtras/Linux_MinecraftApp.cpp
    LinuxExtras/LinuxShutdownManager.cpp
    # Defines LeaderboardManager::m_instance, which every platform owns in its own
    # concrete subclass. Common/Leaderboards/LeaderboardManager.cpp deliberately does
    # not define it - see the comment there.
    Leaderboards/LinuxLeaderboardManager.cpp
    ${MCLINUX_ROOT_SOURCES}
    ${MCLINUX_COMMON_SOURCES}
    ${MCLINUX_UI_SOURCES}
)

target_include_directories(Minecraft.Client.Linux PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(Minecraft.Client.Linux PRIVATE
    Minecraft.World
    # Iggy: the object library must come before nothing in particular here, but note
    # it carries the vendor archive as a PUBLIC dependency and propagates the Iggy
    # header include paths (which stdafx.h needs for iggy.h and gdraw_wgl.h).
    mclinux_iggy
    # Miles: carries the two patched vendor archives and the mss.h include path that
    # Consoles_SoundEngine.h needs (through LinuxMiles.h).
    mclinux_miles
    ${MCLINUX_SDL2_TARGET}
    OpenGL::GL
    GLEW::GLEW
)

# No -Wno- suppressions here either - see the note in Minecraft.World/CMakeLists.txt.
# This target builds warning-free with GCC's default diagnostics.
