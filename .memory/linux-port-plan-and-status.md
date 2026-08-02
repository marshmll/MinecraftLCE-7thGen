# Linux port: plan and live status

Last updated: 2026-07-25, end of Phase 7c — **the game renders and plays correctly and is
actually playable**: movement, mouse look, jump, swimming, block breaking and placing,
creative flight and the HUD all confirmed working by the project owner, and it exits
cleanly. If you're resuming this work, read this file first, then
`.claude/linux-port/ARCHITECTURE.md` and `KNOWN_BUGS.md` for the "why" behind everything
below.

## Phase status

| Phase | Status | Summary |
|---|---|---|
| 1 — CMake foundation for `Minecraft.World` | ✅ done | Top-level + `Minecraft.World/CMakeLists.txt`, `_LINUX64` macro |
| 2 — `LinuxStubs.h` + World portability fixes | ✅ done | Win32-compat shim, repo-wide backslash/case-sensitivity fixes, several genuine pre-existing bugs fixed |
| 3 — Linux client shell | ✅ done | SDL2 window + GL context + event pump, stubbed middleware |
| 4 — `LinuxRender` (real OpenGL) | ✅ done | Full `C4JRender` OpenGL 3.3 implementation, verified with a rendered rotating triangle |
| 5 — Input/Storage/Audio | ✅ done | `LinuxInput`/`LinuxStorage`/`LinuxAudioShim`, verified standalone |
| 6 — Docs and cleanup | ✅ largely done | `.claude/linux-port/*`, `.memory/*`, `CLAUDE.md` written and kept current through Phase 7 |
| 7 — Boot the real game (skip Iggy) | ✅ done | Renders correctly, playable, exits cleanly. Remaining items are polish — see "still open" |
| 8 — Iggy on Linux | ✅ done | **Not a replacement — the real Iggy runs on Linux and is wired into the client.** Frontend boots, real HUD renders; see `.claude/linux-port/IGGY.md` |

Phase 7 was split into sub-steps as it grew far larger than expected:
- **7a** (trunk gameplay files compiling) — ✅ done. The entire non-Iggy
  `Minecraft.Client` gameplay/rendering tree now compiles and links.
- **7b** (real bootstrap, replacing the test-triangle demo) — ✅ done. Binary boots
  into a real generated world via the direct `TemporaryCreateGameStart()` path.
- **7c** (fix what live playtesting revealed) — ✅ done. 19 distinct bugs, listed below.


## What's confirmed working right now

- Clean build: `cmake -B build -S . && cmake --build build` — 0 errors.
- Boots, generates a real world, real `MinecraftServer` with correct settings, real
  texture/archive loading.
- **The game renders correctly.** Verified by screenshot: correctly positioned blocky
  terrain, crisp pixelated block textures, correct colours (grass green, dirt brown,
  sand tan, stone grey, water blue, birch bark white/black), trees with leaves, sky and
  clouds, and a correctly pink pig. ~98 fps.
- **Clean exit.** Window close / `SIGTERM` reaches `Shutdown manager: Complete.` and the
  process terminates. Verified over three consecutive runs with no new coredump.
- Keyboard movement (WASD) and mouse-look wired end-to-end in `LinuxInput.cpp`.

### Fixed this session (details in `.claude/linux-port/KNOWN_BUGS.md`)

All in `LinuxRender.cpp` unless noted:

1. Command buffers recorded only `DrawVertices`, discarding the per-chunk
   `translateToPos()` — every chunk drew stacked in one 16³ box. Now records all
   matrix/state commands.
2. Renderer state was unsynchronized globals but recorded from 4 threads — the reported
   `malloc(): unaligned tcache chunk` abort. Now `thread_local` recording + a
   mutex-guarded store.
3. `VERTEX_TYPE_COMPRESSED` was skipped entirely, so **no terrain geometry reached the
   GPU** — it is the format all terrain uses. Layout derived from `Tesselator::vertex()`
   and validated with a standalone round-trip harness.
4. `StateSetFaceCullCW` mapped to `glFrontFace` instead of `glCullFace` — blocks
   rendered inside out.
5. `TextureBindVertex` (the lightmap) shared a slot with `TextureBind` — everything
   sampled a 16×16 light texture instead of the 256×256 atlas.
6. `TextureSetParam` targeted the wrong slot, so the lightmap's `LINEAR`/`CLAMP` landed
   on the terrain atlas — blurry instead of pixelated blocks.
7. `LoadTextureData` handed stb's RGBA bytes to an engine that expects packed ARGB ints
   — red/blue transposed everywhere (water red, pigs blue).
8. Vertex colour byte order (`a_colour.wzyx` in the vertex shader).
9. Quads now batch through one `glDrawElements` instead of a draw call per quad;
   `CBuffSize()` made O(1).
10. `Chunk::~Chunk()` unconditionally deleted a non-owned `AABB*` (uninitialised in the
    default ctor, aliased by `makeCopyForRebuild`) — heap abort at exit. Added
    `Chunk::ownsBB` (`Chunk.cpp`/`Chunk.h`).
11. Escape quit the game (`Linux_App.cpp`); shutdown then hung on a negative thread
    count (`MinecraftServer.cpp` `#if __PS3__` guard + `LinuxShutdownManager.cpp`
    predicate and `std::atomic`). Together these were the reported "freeze".


### Also fixed this session — gameplay/input (details in `KNOWN_BUGS.md`)

12. **Camera could not turn at all.** `InitGameSettings()` only called `SetDefaultOptions()`
    under `#if _WINDOWS64` / `#elif` the four consoles, so `GAME_SETTINGS` stayed zeroed and
    `ucSensitivity == 0` made the look axes permanently 0 (`Consoles_App.cpp`).
13. **Underwater viewport glitch + wrong FOV everywhere.** `MatrixPerspective` treated
    `gluPerspective`'s degrees as radians (`LinuxRender.cpp`, plus the smoke-test call site).
14. **The game clock had 1-second resolution.** `GetSystemTime()` hardcoded
    `wMilliseconds = 0` and `SystemTimeToFileTime()` dropped the field, so
    `System::currentTimeMillis()` jumped in 1000ms steps and tick loops ran in bursts —
    the "mobs update once a second, very spiky" report (`LinuxStubs.cpp`).
15. **Jump was blocked by tutorial input constraints.** `s_bProfileIsFullVersion` was never
    initialised, so `IsFullVersion()` was false, so the game built a `TrialMode` (derived
    from `FullTutorialMode`). Its `isInputAllowed()` constrains jump and can never lift
    because the Iggy UI that advances the tutorial is bypassed. Water was the exception only
    because `Tutorial.cpp:1964` has an eye-level "allow keypresses so they can jump out of
    water" escape hatch — whose flickering at the surface also explains the intermittent
    swim impulse and the spurious flight toggles (`Extrax64Stubs.cpp`).
16. **The whole in-game input block and the HUD were disabled.** A failed Iggy scene
    navigation latches "menu displayed" on forever; `Minecraft.cpp:2238` gates all in-game
    input on it (breaking/placing) and `Gui.cpp:175` gates the HUD on it. Neutralised at
    both layers (`LinuxUIController.h`, `LinuxInput.cpp`). **The HUD had been missing for
    all of Phase 7 because of this and nothing reported an error.**
    *(Both neutralisations were reverted in Phase 8: scenes now build, so the flag
    behaves as designed and honouring it is correct - otherwise the camera turns and the
    player walks underneath an open menu.)*
17. **WASD also fired debug D-pad actions** (W = fly toggle, S = debug overlay,
    A = spawn creeper, D = change skin) because the keyboard raised `DPAD_*` bits alongside
    the stick bits. This was the real cause of the long-standing "flying is on" and
    "W and D feel swapped" reports (`LinuxInput.cpp`).
18. Mouse look redesigned: non-destructive velocity sampling with `sqrt` pre-compensation
    for `Input.cpp`'s quadratic response; `LY`/`RY` corrected to forward/up-positive (W/S
    were inverted). Mouse break/place bindings were swapped (left now breaks).
19. SHIFT wired to sneak / descend-while-flying.

## What's still open — pick up here

Everything reported by live playtesting is now fixed and confirmed by the project owner:
movement, mouse look, jump, swimming, breaking, placing, flight, HUD, clean exit.

Remaining known gaps, none currently blocking play:

1. **The lightmap is tracked but never sampled.** `TextureBindVertex`'s texture is kept out
   of the fragment slot but not used, and `ExpandCompressedVertices` drops the secondary UVs
   (`[6..7]`) it would need. Per-block light comes only from baked vertex colours, so there
   is no smooth torch falloff. This is the main remaining *rendering-correctness* gap.
2. ~~**`glColor4f` is ignored for tesselated draws.**~~ **FIXED 2026-08-01.** `StateSetColour`
   set attribute 2's *default* value, which only applies when that attribute's array is
   disabled — and all four arrays are permanently enabled, so it was silently dropped for
   every tesselated draw. Now a `u_colourMul` uniform multiplied into the vertex colour in
   the vertex shader. The two cases compose correctly: `Tesselator::end()` writes white
   when `hasColor` is false (so `glColor4f` wins — this is how grass/foliage/water get
   their biome tint), and callers that do supply per-vertex colours leave `glColor4f` at
   white. The visible symptom was **grey grass in item icons**; confirmed fixed by the
   project owner.
3. Clouds look blocky/stretched; leaf cutouts show white speckles (alpha test only
   implements the `GL_GREATER` case); the held item renders as a large flat shape.
4. The player sometimes spawns inside terrain — unclear whether a real spawn-placement bug
   or just the direct `TemporaryCreateGameStart()` path skipping normal placement.
5. **Residual: threads that never exit** (`rebuildChunkThreadProc`, `runSaveThreadProc`) —
   see `KNOWN_BUGS.md`. Latent; exit is clean and repeatable today.
6. Phase 6 (docs/cleanup) is effectively done as a side effect of this work.

## Phase 8 — Iggy on Linux (2026-07-26)

The premise that Iggy was unavailable on Linux was **wrong**: its OpenGL GDraw backend
ships as source in `Windows64/Iggy/gdraw/`, and `Orbis/Iggy/lib/libiggy_orbis.a` (the
PS4 core) is ELF x86-64 with the System V ABI. So the vendor core is reused rather than
reimplemented — reimplementing would mean writing a Flash player with an AVM2
interpreter, since the UI's layout, focus and hit-testing all live in 9.4 MB of
ActionScript inside the 351 SWF assets and C++ only drives it by RPC.

**Done — the real LCE UI now runs in the client.**

- Booting brings up the authentic intro: panorama, Minecraft logo, autosave message box
  with real string-table text, focus highlight, "Ⓐ Select" prompt. 8 scenes, 0 import
  errors.
- Input round-trips C++ → SWF → AS3 → C++ (`handlePress`, `New Stage focus Button1..3`).
- `--direct-world` closes the frontend and launches a world directly (the old Phase 7
  path), and now shows the **real Iggy HUD** — hotbar, selection highlight, crosshair,
  "Ⓧ Creative / Ⓨ Inventory". That HUD was missing for all of Phase 7; `Gui.cpp`'s
  `#define RENDER_HUD 0` was never the gap, the Iggy HUD was always the intended one.
- Standalone `iggy_spike` still validates all 335 SWFs headless and can render any
  single scene to a PNG.

**Two things dominated the effort, and neither is a build-system problem:**

1. **2461 relocations.** Sony emits `R_X86_64_PLT32_BND` for GOT loads; GNU ld resolves
   it as direct PC32, so every global access read the variable instead of its address.
   `IggyInit` segfaulted on instruction #3. Rewriting to `R_X86_64_REX_GOTPCRELX` fixed
   it.
2. **`wchar_t` is 32-bit on Linux, `IggyUTF16` is 16-bit.** All 93 `string16` sites in
   `Common/UI` pass wide strings by reinterpret cast, which only works because MSVC's
   `wchar_t` is 16-bit. Symptom: a skin library logs 2 MB loaded, then every scene fails
   with "Attempted to import undefined library" for that same library — it registered
   under a one-character URL. Fixed with `LinuxIggyUtf16.cpp`, marshalling at 5 renamed
   Iggy entry points instead of editing 93 call sites.

Both were found by reading bytes — relocation type numbers, struct offsets — not by
reasoning about the code, which was confidently wrong each time.

Remaining: polish only. Profile-backed elements (`LabelGamertag`/`PlayerName`) have no
profile behind them; one scene lacks a `SetSafeZone` AS3 method; mouse pointer and text
entry are unwired (menus navigate by focus/keycode so neither blocks); and the deeper
screens (world select, create-world, inventory, pause) compile and their SWFs load but
have not been driven interactively.

Read `.claude/linux-port/IGGY.md` before touching any of this.

## Phase 8b — what live playtesting of the frontend found (2026-08-01)

The frontend booted, but four things were reported: black intro, button prompts with no
glyph art, no autosave spinner, and "create world, leaderboard, all of them" crashing plus
a skin-select menu that rendered no skins. These were **three unrelated groups**, not one
bug.

**Fixed:**

1. **`memset` over a `std::wstring`** — `UIStructs.h:268` zeroed a struct containing
   `wstring seed`, then assigned `seed = L""`, writing through libstdc++'s nulled pointer.
   MSVC survives it; glibc does not. Killed the constructor of every screen deriving from
   `IUIScene_StartGame`, i.e. **both create-world and load-world**. Confirmed by coredump
   42660. Only instance of the pattern in the tree.
2. **`LeaderboardManager::Instance()` was NULL** — the singleton is defined once per
   platform in that platform's own subclass; Linux had no `Leaderboards/` directory. Added
   `Linux/Leaderboards/LinuxLeaderboardManager.{h,cpp}` mirroring Windows64's 36-line stub
   and removed the `= NULL` that an earlier session had put in the *shared* file (which
   turned a link error naming the culprit into a segfault). Also hit in-game via
   `StatsCounter::flushLeaderboards()`.
3. **Custom draw had no `_LINUX64` branch** (`UIController.cpp:1232`) — no
   `RenderManager.StartFrame()`, no GDraw viewport reset, so the `glOrtho` below described
   a pixel space that did not match the bound viewport. This is why **skin previews and all
   item icons** were missing. Needed a new `gdraw_GL_setViewport_4J()` in `gdraw_sdl.c`.
4. **Worlds were never written to disk** — `ConsoleSaveFileOriginal.cpp:785`/`:809` and the
   header's `:35` omitted `_LINUX64`, so `SetSaveImages`/`SaveSaveData` never ran and
   nothing reported an error.
5. Four more missing-`_LINUX64` guards: Iggy font-cache buffer sized 16 B/char instead of
   24 (`UIController.cpp:2591`, which Iggy had been warning about every boot), an
   uninitialised platform id handed to the Controls screen's AS3
   (`UIScene_ControlsMenu.cpp:15`), oversized inline button glyphs
   (`Consoles_App.cpp:6247`/`:6413`), and two create-world dead ends
   (`UIScene_CreateWorldMenu.cpp:602`/`:622`).
6. Deleted the dead `LinuxIggyShim.cpp`; verified with `nm` that the linked
   `IggyPlayerReadyToTick` is the vendor's.

**Also fixed, after two wrong diagnoses (2026-08-01, later the same day):**

7. **The frontend never cleared the depth buffer.** Iggy encodes an object id in the depth
   buffer - `depth_from_id()` plus `test_id`/`set_id` with `glDepthFunc(GL_LESS)`
   (`gdraw_gl_shared.inl:1269`, `:1479-1490`); per-draw tracing confirmed the button-icon
   quads arrive with `test_id=1 set_id=1`. In-game the world renderer clears depth
   (`GameRenderer.cpp:1291`), but with no world running nothing did, so ids accumulated and
   every depth-tested draw failed from the second frame on. **One bug behind three
   symptoms**: button prompts drawing their letter but not the disc behind it, the autosave
   icon never appearing, and the black intro. Fixed with a `RenderManager.Clear(GL_DEPTH_BUFFER_BIT)`
   at the top of `LinuxUIController::render()`. Verified: disc, autosave chest icon and the
   ESRB intro screen all render.
8. **The custom-draw matrix was column-major.** `gdraw_GetObjectSpaceMatrix`'s last argument
   selects the layout, and the GL backend is the *only* caller in the tree passing 1;
   D3D11/D3D9/Orbis/PS3/PSVita all pass 0, and the shared consumer reads `mat[3]`/`mat[7]`
   for the translation - hard `0.0f` under column-major, so the translate collapsed to
   exactly `(width/2, height/2)`. Item icons and skin previews drew at the screen centre at
   the correct size. Needed three edits, because the vendor `gdraw_GL_BeginCustomDraw`
   recomputes column-major and would undo the fix: flag 0 in `gdraw_GL_CalculateCustomDraw`,
   a new `gdraw_GL_BeginCustomDraw_4J`, and both `LinuxUIController` call sites.
9. **`glColor4f` was dropped for tesselated draws** - see "what's still open" item 2 above.
   Grey grass in item icons.

10. **`IggyCustomDrawCallbackRegion::name` is UTF-16 too** - the Phase 8 `wchar_t` bug in
    the *read* direction. 12 sites in `Common/UI` parse it with `swscanf`/`wcscmp` through a
    `(wchar_t *)` cast, so the hotbar (`"slot_%d"`), all container/crafting/enchanting/
    trading slots, the skin previews (`"Character%d"`) and the main-menu splash never
    matched and drew nothing. `UIScene_HUD` logged "This is not the control we are looking
    for" **30,141 times in a 40s run**; now 0. Fixed by renaming `IggySetCustomDrawCallback`
    in `patch_orbis_iggy.py` and widening the name in a trampoline in `LinuxIggyUtf16.cpp`.
    The tutorial-popup icon had always worked because it never parses the region name,
    which is what disguised this.

11. **`TextureData()` clobbered every texture's sampler state.** `C4JRender::TextureData`
    re-applied `GL_REPEAT` + `GL_NEAREST` on *every upload*, after `glTexImage2D`. But
    `Textures::loadTexture` sets the wrap/filter modes (`Textures.cpp:498-529`) and *then*
    calls `TextureData` to upload (`:584`), so the caller's choice was destroyed a few lines
    later. Entity shadows were the loudest victim: `shadow.png` is loaded as
    `"%clamp%misc/shadow"` and the clamp is load-bearing, because the disc is inscribed in
    the full 64x64 with zero-alpha borders and each ground quad spans exactly one UV period
    (`1/(2*shadowRadius)` == 1.0 at the usual radius 0.5). Clamping is what makes the
    neighbouring tiles transparent; with `GL_REPEAT` each adjacent tile drew another full
    disc, so one mob's shadow leaked outwards as half-circles around the correct one. The
    same clobber also discarded the `GL_LINEAR` that `%blur%` textures ask for. Fixed by
    moving the defaults into `TextureCreate()` (once per texture object) and removing them
    from `TextureData()`. Note GL's own default `MIN_FILTER` (`GL_NEAREST_MIPMAP_LINEAR`)
    is unusable as a fallback here - almost nothing in the tree ships a complete mip chain,
    so those textures would be mipmap-incomplete and sample as opaque black.
12. **`MobRenderer` leaked `glColor4f`.** Its hurt/death and `overlayColor` passes set a
    non-white colour and the tail restored depth/blend/alpha/texturing but not colour, so
    the tint bled into the next draw - most visibly the entity's own shadow, which
    `postRender()` draws immediately after. Harmless until item 9 above made `glColor4f`
    effective; now restored alongside the other state.

**Two wrong diagnoses recorded here first, both instructive:**

- *"Bitmap-filled `DefineShape`s never rasterise."* Refuted by an in-game screenshot: the
  X/Y/RT prompts are `DefineShape`s with the same `0x43` bitmap fill, and the disc that
  failed is byte-for-byte the same construction as one that worked. The error was comparing
  two different prompts in two different contexts and blaming the asset. **The
  frontend-vs-in-game split was the actual clue** and pointed at per-frame state.
- *"The intro is fast-forwarded by `while(IggyPlayerReadyToTick())`."* Inferred from three
  log lines being adjacent - but nothing logs between them, so **adjacency in a log without
  timestamps says nothing about elapsed time.** The owner confirmed the timing was fine.

**New tool:** `MCLINUX_GL_DEBUG=1` installs a KHR_debug callback in `gdraw_sdl.c`. Before
this, GDraw's `opengl_check()` was compiled out (`_DEBUG` is not defined for that target
because its error path is `RR_BREAK()` = `int $3`), so **every GL error inside GDraw was
silently discarded and a clean log proved nothing.**
