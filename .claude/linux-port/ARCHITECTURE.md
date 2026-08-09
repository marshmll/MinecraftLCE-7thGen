# Linux port architecture

## Why this is hard: closed-source middleware

4J Studios shipped four subsystems as closed-source, Windows-only prebuilt `.lib`s,
with only headers in the leak (`Minecraft.Client/Windows64/4JLibs/inc/*.h`):

| Header | Class | Global singleton | Real Linux implementation |
|---|---|---|---|
| `4J_Render.h` | `C4JRender` | `RenderManager` | `Minecraft.Client/Linux/LinuxExtras/LinuxRender.cpp` — full OpenGL 3.3 core backend |
| `4J_Input.h` | `C_4JInput` | `InputManager` | `Minecraft.Client/Linux/LinuxExtras/LinuxInput.cpp` — SDL2 gamepad + keyboard/mouse |
| `4J_Storage.h` | `C4JStorage` | `StorageManager` | `Minecraft.Client/Linux/LinuxExtras/LinuxStorage.cpp` — local save/load only |
| `4J_Profile.h` | `C_4JProfile` | `ProfileManager` | Not reimplemented — see below, it didn't need to be |

All four are plain concrete classes with **no virtuals and no source** — every method
body lived only in the vendor `.lib`. Porting means writing a brand-new `.cpp` that
implements every declared method for real, then defining the global singleton.

`Minecraft.Client/stubs.h`/`glWrapper.cpp` already exist as a legacy LWJGL/GL-style
free-function call surface (`glMatrixMode`, `glClear`, `glBindTexture`, ...) that
forwards straight to `RenderManager.<Method>()` — this is platform-generic and was
**not** touched; only which `C4JRender` implementation backs `RenderManager` changes
per platform.

### `4J_Profile.h` turned out to be a non-issue

Unlike the other three, a complete stub implementation of every `C_4JProfile` method
already exists in the leak at `Minecraft.Client/Extrax64Stubs.cpp` (~lines 382-541),
originally guarded `#ifdef _WINDOWS64` (widened to also cover `_LINUX64`). This file
also has fake `IQNet`/`IQNetPlayer` networking bodies under the same guard, backing
`PlatformNetworkManagerStub` — real sockets were never needed for a single-player
Linux boot.

### Header-adaptation precedent: when to fork a Linux-specific copy

Two of the four headers needed a Linux-specific copy under
`Minecraft.Client/Linux/4JLibs/inc/`, because their signatures genuinely couldn't be
satisfied by reusing Windows64's copy verbatim:

- **`4J_Render.h`**: `Initialise(ID3D11Device*, IDXGISwapChain*)` assumed a D3D11
  device/swapchain; Linux's `Initialise()` takes an opaque `void*` window handle
  instead (cast to `SDL_Window*` internally — the header itself doesn't depend on
  SDL2, since `Minecraft.World`'s `stdafx.h` includes it and `Minecraft.World`
  doesn't link SDL2).
- **`4J_Storage.h`**: `ConsoleSaveFileSplit.cpp` calls a Subfile save API
  (`GetSaveState`/`GetSubfileCount`/`AddSubfile`/etc.) that Windows64's/PS3's copies
  of this header don't declare — only Orbis/Durango's do. This was a pre-existing
  gap unrelated to Linux; the Linux copy adds those methods matching
  Orbis/Durango's signatures.

- **`4J_Input.h`**: its surface has no D3D/Win32-only types, so Windows64's copy was
  reused unchanged for most of the port. It is forked now because three desktop inputs
  have no console equivalent to express them through: raw mouse look (`ConsumeMouseLook`),
  the mouse wheel and the number row (`ConsumeMouseWheel`/`ConsumeHotbarSlotRequest`).
  A stick axis means a turn *rate* and there is no absolute-hotbar-slot action at all —
  see `KNOWN_BUGS.md`.

`4J_Profile.h` is the only one still reused from Windows64 verbatim.

## `C4JRender`'s contract, as reverse-engineered from its callers

`LinuxRender.cpp` is the largest guess in this port: no vendor implementation exists, so
every method's semantics had to be inferred. Several inferences turned out wrong in ways
that produced rendering bugs (all fixed — see `KNOWN_BUGS.md`). The parts of the contract
now established by evidence, worth knowing before touching that file:

- **Command buffers are real display lists.** `glWrapper.cpp` maps
  `glNewList`/`glEndList`/`glCallList` onto `CBuffStart`/`CBuffEnd`/`CBuffCall`, and
  callers put matrix and state commands *inside* the pair, not just geometry. A recording
  call must therefore record and **not** execute — which is also the correct `GL_COMPILE`
  semantic. `Chunk::rebuild`'s per-chunk `translateToPos()` lives inside its list and is
  the only thing that positions a chunk in the world.
- **Recording is concurrent.** Four threads record command buffers while the main thread
  replays them; each recording thread announces itself by calling `InitialiseContext()`
  on entry. Recording state must be per-thread; the shared store needs a lock. Worker
  threads never have a GL context, which is fine because recording touches no GL.
- **There are two texture slots, not one.** `TextureBind` is the fragment-stage texture;
  `TextureBindVertex` is the lightmap. `TextureData`/`TextureDataUpdate`/`TextureSetParam`
  act on whichever was bound *most recently*, which is how
  `GameRenderer::turnOnLightLayer` sets filters on the lightmap after binding it.
- **Terrain vertices are `VERTEX_TYPE_COMPRESSED`**, a 16-byte packed format, not the
  32-byte one. Its layout has no header or shader in the leak and was recovered from
  `Tesselator::vertex()`.
- **Colour byte order is big-endian-oriented throughout** — both vertex colours
  (`Tesselator` packs `(r<<24)|(g<<16)|(b<<8)|a`) and texture pixels (the engine treats
  loaded images as packed ARGB ints). A Linux loader handing over stb's natural RGBA byte
  order transposes red and blue for the whole game.

## The Iggy wall — turned out not to be a wall (see `IGGY.md`)

The entire modern UI scene system — `Minecraft.Client/Common/UI/*`, ~109 files, the
title screen / world-select / create-world / pause menu / inventory / options
screens, driven by the `ui` global (`ConsoleUIController`/`UIController`) — is
unconditionally built on **Iggy**, RAD Game Tools' closed-source vector-UI engine
(`#define _ENABLEIGGY` is unconditional in `Windows64_UIController.cpp`).

This section used to say Iggy was "prebuilt Windows-only `.lib`/`.dll`, zero source,
zero Linux equivalent … a bigger, harder wall than Render/Input/Storage/Profile
combined". **That was wrong.** Two things it missed:

- Iggy's **OpenGL** GDraw backend ships as *source* in this tree
  (`Windows64/Iggy/gdraw/gdraw_gl_shared.inl`), built by no `.vcxproj` and containing
  no Windows API calls.
- `Orbis/Iggy/lib/libiggy_orbis.a` — the PS4 build of the portable core — is **ELF
  x86-64 with the System V ABI**, unstripped, and therefore linkable on Linux.

So Iggy did not need reimplementing (which would have meant writing a Flash player with
an AVM2 interpreter — all layout, focus and hit-testing lives in 9.4 MB of ActionScript
inside the SWF assets). It needed a platform layer and some SDL glue. That now exists
under `Minecraft.Client/Linux/Iggy/`: all 335 SWFs load and run their ActionScript, and
real menu screens render through Iggy's own GL backend with zero GL errors.

**Read `IGGY.md` before touching any of it** — in particular the 2461 relocations that
must be rewritten, and the 96-byte `jmp_buf`.

**The client links it now, and the bypass has been unpicked** — the frontend boots
through real Iggy scenes and `TemporaryCreateGameStart()` survives only behind
`--direct-world`. The two facts below are what made the *original* bypass tractable; the
first is still load-bearing (it is why `ui.render()` can be bracketed as one region), the
second is now only a debug shortcut:

1. **The in-game HUD and 3D world rendering are Iggy-free.**
   `GameRenderer::render()` draws the world via `LevelRenderer` then calls
   `mc->gui->render(...)` directly — `Gui`/`GuiComponent` (crosshair, hotbar,
   health/hunger/XP, chat, F3 debug text) draw straight through
   `Tesselator`/`RenderManager`, an old self-contained path with zero Iggy calls.
   `ui.render()` is a separate step in the main loop, never nested inside world/HUD
   rendering.
2. **There's a real, already-functional code path into a loaded world that skips all
   Iggy UI.** Normally triggered by `UIScene_CreateWorldMenu`'s
   `StartSharedLaunchFlow()`; the actually-usable shortcut is
   `CConsoleMinecraftApp::TemporaryCreateGameStart()`
   (`Minecraft.Client/Windows64/Windows64_App.cpp`, permanently dead/commented-out
   code on every real platform, but real, working logic) — build a
   `NetworkGameInitData`, call `g_NetworkManager.HostGame(...)` +
   `FakeLocalPlayerJoined()`, then `CGameNetworkManager::RunNetworkGameThreadProc` →
   `MinecraftServer::main(...)` → client-side `setLevel(...)`. Ported into
   `Minecraft.Client/Linux/LinuxExtras/Linux_MinecraftApp.cpp`'s own
   `TemporaryCreateGameStart()`.

`Minecraft::run_middle()`/`tick()` still have a **shallow** (not rendering) `ui`
dependency — menu-state queries, scene navigation, tooltips, UI sound effects
(`GetMenuDisplayed`, `IsPauseMenuDisplayed`, `NavigateToScene`, `PlayUISFX`,
`SetTooltips`, `HandleGameTick`). A no-op `UIController` stand-in
(`Minecraft.Client/Linux/LinuxExtras/LinuxUIController.h/.cpp`) satisfies exactly this
surface — real Iggy asset loading never happens. A small subset of `Common/UI/*`
(base infrastructure — `UIController`/`UIGroup`/`UILayer`/`UIScene`, a handful of
non-Iggy "component" overlays like the HUD/tooltips/debug-console popups, and the
`IUIScene_*` container-family mixins) IS compiled in, because the trunk transitively
needs it; the ~100 concrete `UIScene_*.cpp`/`IUIScene_*` **menu screens** are not.
> **The two paragraphs above are HISTORICAL.** They describe the Phase 7 shape, kept
> because the seam they document is still the seam. As of Phase 8 all of it is superseded:
> the whole of `Common/UI/*` is compiled (minus the two files the vcxproj itself excludes),
> `LinuxUIController` is a real controller driving real Iggy scenes, and
> `LinuxIggyShim.cpp` has been **deleted** — the client links the vendor Iggy core, so
> those no-op bodies would be duplicate definitions of the real entry points.
> `.claude/linux-port/IGGY.md` is the current description; read it instead of this section.

## Other singletons a boot needs

Beyond the four 4J middleware singletons, the boot sequence also needs:

- **`app`** (`CConsoleMinecraftApp`-shaped) → `Linux_MinecraftApp.h/.cpp`
- **`ui`** (`ConsoleUIController`-shaped) → `LinuxUIController.h/.cpp` — a real
  controller since Phase 8, driving the vendor Iggy core through GDraw's GL backend
- **`SentientManager`** (telemetry) — trivial no-op; only one live (non-commented-out)
  call site exists in the entire non-console codebase
- **`g_NetworkManager`** (`CGameNetworkManager`) — already has a working stub backend
  (`PlatformNetworkManagerStub` + `Extrax64Stubs.cpp`'s fake `IQNet`), no Linux work
  needed beyond widening the `_WINDOWS64` guard
- **`TelemetryManager`** — a pointer, not dereferenced unconditionally by anything in
  the compiled trunk; left uninitialized/null

## Boot sequence (`Minecraft.Client/Linux/Linux_Minecraft.cpp`)

Mirrors `Windows64_Minecraft.cpp`'s real shape, with SDL2 replacing
Win32/D3D11/`PeekMessage`, and the Iggy title-screen flow replaced by the direct
launch above:

1. SDL2 window + GL 3.3 **compatibility** context (`Linux_App.cpp`) - compatibility is
   required by GDraw's GL backend, not a preference; see `IGGY.md`
2. `RenderManager.Initialise(window handle)`
3. `app.loadMediaArchive()` / `app.loadStringTable()` — needs a real `_LINUX64`
   branch (added in `Common/Consoles_App.cpp`) to find `Common/Media/MediaWindows64.arc`
4. `ui.init(width, height)` — `preInit()` → `gdraw_GL_SetResourceLimits` →
   `gdraw_GL_CreateContext` → `IggySetGDraw` → `postInit()`, which ends by navigating to
   `eUIScene_Intro`. (Was `ui.Boot()` in Phase 7, when every Iggy call inside resolved to
   a no-op shim; that shim is gone and these are real calls now.)
5. `InputManager.Initialise(...)` + `DefineActions()` (ported verbatim from
   `Windows64_Minecraft.cpp` — populates the joypad action-map table so
   `GetValue(MINECRAFT_ACTION_*)` resolves to real button bits)
6. `ProfileManager.Initialise(...)`, `StorageManager.Init(...)`
7. Per-thread TLS setup every platform's own entry point does before
   `Minecraft::main()` (`Tesselator`/`AABB`/`Vec3`/`IntCache`/`Compression`/
   `OldChunkStorage`/`Tile::CreateNewThreadStorage()`)
8. `Minecraft::main()` (constructs the `Minecraft` singleton — must run **before**
   step 9, which dereferences it)
9. `g_NetworkManager.Initialise()`, then `app.TemporaryCreateGameStart()` — the
   direct non-UI world launch
10. Real per-frame loop: `RenderManager.StartFrame()` → `InputManager.Tick()` →
    `StorageManager.Tick()` → `RenderManager.Tick()` → `pMinecraft->run_middle()`
    (drives `GameRenderer::render()` → world + HUD) → `ui.tick()`/`ui.render()`
    (no-ops) → `RenderManager.Present()`
11. On window close: `ShutdownManager::StartShutdown()` +
    `ShutdownManager::MainThreadHandleShutdown()` — see `KNOWN_BUGS.md` for why this
    is load-bearing, not optional cleanup.

A `--smoke-test` CLI flag keeps the original Phase 3-5 standalone test (rotating
triangle + throwaway input/storage/audio self-tests) reachable as a regression check
for those subsystems in isolation from real game logic.
