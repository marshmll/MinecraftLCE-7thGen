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

# OpenAL-soft backs LinuxAudioShim.cpp's AIL_* (Miles Sound System)
# signature-compatible shim - see that file for scope/fidelity notes.
find_package(OpenAL QUIET)
if(NOT TARGET OpenAL::OpenAL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPENAL REQUIRED IMPORTED_TARGET openal)
    add_library(OpenAL::OpenAL ALIAS PkgConfig::OPENAL)
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

set(MCLINUX_UI_SOURCES
    Common/UI/UIController.cpp
    Common/UI/UIGroup.cpp
    Common/UI/UILayer.cpp
    Common/UI/UIScene.cpp
    # Small "component" overlays (HUD/tooltips/debug console/press-start/
    # tutorial popup) - referenced directly by UIController/UIGroup, not just
    # via the disabled addComponent() factory switch; not Iggy-only screens.
    Common/UI/UIComponent_DebugUIConsole.cpp
    Common/UI/UIComponent_PressStartToPlay.cpp
    Common/UI/UIComponent_Tooltips.cpp
    Common/UI/UIComponent_TutorialPopup.cpp
    Common/UI/UIScene_HUD.cpp
    # Base widget/font classes the component overlays above build on -
    # generic UI building blocks, not concrete Iggy-menu screens.
    Common/UI/UIControl.cpp
    Common/UI/UIControl_Base.cpp
    Common/UI/UIControl_Label.cpp
    Common/UI/UIBitmapFont.cpp
    Common/UI/UITTFFont.cpp
    Common/UI/UIFontData.cpp
    # Referenced directly by IUIScene_StartGame.cpp (DLC texture-pack list -
    # part of the non-Iggy IUIScene_* container-logic set below, not a
    # concrete menu screen itself); their own bodies have no Iggy calls
    # beyond what UIControl_Base.cpp (already included) already covers.
    Common/UI/UIControl_BitmapIcon.cpp
    Common/UI/UIControl_TexturePackList.cpp
    # Referenced directly by UIScene_FullscreenProgress.cpp (progress bar +
    # cancel button widgets) - same non-Iggy-screen "control widget" category
    # as UIControl_BitmapIcon/TexturePackList above.
    Common/UI/UIControl_Progress.cpp
    Common/UI/UIControl_Button.cpp
    # Referenced directly (not just via the disabled scene-factory switch)
    # from UILayer.cpp and Common/Network/GameNetworkManager.cpp - genuinely
    # load-bearing infrastructure, not one of the ~100 excluded concrete
    # Iggy menu screens.
    Common/UI/UIScene_FullscreenProgress.cpp
)
list(TRANSFORM MCLINUX_UI_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/../")

# IUIScene_* (container-family mixins - AbstractContainerMenu/AnvilMenu/
# BrewingMenu/etc, 13 files) are a SEPARATE, smaller set from the ~100
# concrete UIScene_*.cpp menu screens: base non-Iggy container logic the
# component overlays/UI infrastructure above pull in transitively. Already
# absolute paths from file(GLOB ...) - appended after the PREPEND above so
# they don't get double-prefixed.
file(GLOB MCLINUX_IUISCENE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../Common/UI/IUIScene_*.cpp")
list(APPEND MCLINUX_UI_SOURCES ${MCLINUX_IUISCENE_SOURCES})

# The ~56 per-entity *Renderer.cpp / ~34 *Model.cpp / ~29 *Particle.cpp files
# are already covered by MCLINUX_ROOT_SOURCES's glob above.

add_executable(Minecraft.Client.Linux
    Linux_App.cpp
    Linux_Minecraft.cpp
    LinuxExtras/LinuxRender.cpp
    LinuxExtras/LinuxInput.cpp
    LinuxExtras/LinuxStorage.cpp
    LinuxExtras/LinuxAudioShim.cpp
    LinuxExtras/LinuxIggyShim.cpp
    LinuxExtras/LinuxUIController.cpp
    LinuxExtras/Linux_MinecraftApp.cpp
    LinuxExtras/LinuxShutdownManager.cpp
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
    ${MCLINUX_SDL2_TARGET}
    OpenGL::GL
    GLEW::GLEW
    OpenAL::OpenAL
)

# Same dead-code ListTag<T>::print()/Tag::print() ostream/wostream mismatch
# already suppressed for Minecraft.World (see its CMakeLists.txt) - the
# trunk's headers pull ListTag.h in too.
target_compile_options(Minecraft.Client.Linux PRIVATE -Wno-template-body)
