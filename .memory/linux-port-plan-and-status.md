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
| 8 — Iggy replacement (deferred) | not started | Explicitly out of scope until Phase 7 lands cleanly |

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
2. **`glColor4f` is ignored for tesselated draws.** `StateSetColour` sets a default vertex
   attribute, but attribute 2 is a permanently enabled array, so per-vertex colour always
   wins (and `Tesselator::end()` writes white when `hasColor` is false). Affects overlay
   brightness — e.g. the in-wall overlay is far brighter than intended.
3. Clouds look blocky/stretched; leaf cutouts show white speckles (alpha test only
   implements the `GL_GREATER` case); the held item renders as a large flat shape.
4. The player sometimes spawns inside terrain — unclear whether a real spawn-placement bug
   or just the direct `TemporaryCreateGameStart()` path skipping normal placement.
5. **Residual: threads that never exit** (`rebuildChunkThreadProc`, `runSaveThreadProc`) —
   see `KNOWN_BUGS.md`. Latent; exit is clean and repeatable today.
6. Phase 6 (docs/cleanup) is effectively done as a side effect of this work; Phase 8 (Iggy
   replacement) remains deliberately out of scope.

## Method note for whoever picks this up

Two techniques did essentially all the work this session, and neither is "read the code and
reason about it" — that produced a confident wrong answer nearly every time:

1. **Print the gate, not the symptom.** Every input bug was found by printing the two halves
   of a single `if` and seeing which was false. `isInputAllowed` vs `GetValue`;
   `screen == NULL` vs `GetMenuDisplayed`. Minutes each, after hours of failed theorising.
2. **Instrument and read numbers for rendering.** Bound texture id and size, vertex colour
   bytes, UVs, GL filter enums. "Terrain looks untextured" measured as a correctly-textured
   draw sampling a 16x16 lightmap; "colours are wrong" measured as *correct* grey vertex
   colours times a channel-swapped texture.

Also: the project owner's playtest reports are precise and worth taking literally. "It stops
the exact moment my camera gets out of the water" pinned an eye-level test
(`isUnderLiquid`) that no amount of code reading had suggested; "bouncing triggers flying"
identified a rising-edge double-tap detector firing on a flickering gate.
