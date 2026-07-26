# Linux port: plan and live status

Last updated: 2026-07-25, late Phase 7c — **the game now renders and plays correctly**
(screenshot-verified: textured terrain, correct colours, trees, clouds, a pink pig) and
exits cleanly. If you're resuming this work, read this file first, then
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
| 6 — Docs and cleanup | ⏳ not started | This session's `.claude`/`.memory`/`CLAUDE.md` work is a head start on this |
| 7 — Boot the real game (skip Iggy) | 🔶 in progress | Renders correctly and exits cleanly; what's left is rendering polish + an input re-test, see below |
| 8 — Iggy replacement (deferred) | not started | Explicitly out of scope until Phase 7 lands cleanly |

Phase 7 was split into sub-steps as it grew far larger than expected:
- **7a** (trunk gameplay files compiling) — ✅ done. The entire non-Iggy
  `Minecraft.Client` gameplay/rendering tree now compiles and links.
- **7b** (real bootstrap, replacing the test-triangle demo) — ✅ done. Binary boots
  into a real generated world via the direct `TemporaryCreateGameStart()` path.
- **7c** (fix what live playtesting revealed) — 🔶 in progress. The rendering and
  stability bugs are done; see "What's still open" for the remainder.


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

## What's still open — pick up here

Cosmetic/rendering polish, all seen in the final screenshot:

1. **Clouds look blocky/stretched** (user-reported). `LevelRenderer::createCloudMesh`
   (`LevelRenderer.cpp:1350`) records `cloudList`; not investigated.
2. **Leaf cutouts show white/blue speckles** — alpha test may not be discarding
   correctly. `u_alphaTestEnabled`/`u_alphaFunc` only implement the `GL_GREATER` case.
3. **The held item renders as a large flat shape** in the bottom-right corner. Probably
   the `ItemInHandRenderer` display lists / its own matrix setup.
4. **The lightmap is not sampled.** `TextureBindVertex`'s texture is tracked but unused,
   and `ExpandCompressedVertices` drops the secondary UVs (`[6..7]`) it would need. Per-
   block light currently comes only from baked vertex colours, which looks broadly right
   but means no smooth light falloff from torches etc. This is the main remaining
   *rendering-correctness* gap rather than a bug.
5. **The player sometimes spawns inside terrain**, so the first screenshot of a run is
   often the inside of a hill. Unclear whether this is a real spawn-position bug or just
   the direct `TemporaryCreateGameStart()` path skipping normal spawn placement.
6. **Input re-test now due.** The earlier "W and D feel swapped" / "mouse-look doesn't
   turn" / "jump doesn't work" reports were all filed while rendering was broken enough
   to make orientation impossible to judge. Re-test before treating any of them as real.
7. **Residual: threads that never exit** (`rebuildChunkThreadProc`,
   `runSaveThreadProc`) — see `KNOWN_BUGS.md`. Latent, not currently causing failures.

## Method note for whoever picks this up

Every rendering bug this session was **misdiagnosed from the screenshot** and only
pinned down by instrumenting the renderer and reading numbers — bound texture id and
size, vertex colour bytes, UVs, GL filter enums, draw counts. Two examples worth
internalising: terrain "looking untextured" was actually a correctly-textured draw
sampling a 16×16 lightmap, and "colours are wrong" was measured to be *correct* vertex
colours (grey stone) with a channel-swapped texture. Reach for a one-shot `fprintf` in
`ApplyStateAndDraw`/`TextureBind` before theorising. See `BUILD_AND_RUN.md` for the
agent-usable `spectacle` screenshot recipe.
