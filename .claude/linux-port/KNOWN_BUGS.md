# Known bugs and gotchas

## Systemic bug class: closed-middleware seams that "obviously" collapse together

The `C4JRender` API has several pairs of entry points that look interchangeable but are
not. Every one that was collapsed together in `LinuxRender.cpp` produced a rendering bug
that looked like something else entirely, and cost a full debugging cycle each. Before
implementing any new `C4JRender` method as "same as that other one", find its real
callers first.

Found and fixed so far — all four had a wrong *guess* about the interface baked in as a
confident-sounding comment, which is what made them expensive:

| Collapsed | Reality | Symptom |
|---|---|---|
| `CBuff*` recorded only `DrawVertices` | Display lists carry matrix/state commands too | Every chunk drawn stacked in one 16³ box |
| `TextureBindVertex` → same slot as `TextureBind` | It's the **lightmap**, a second slot | Everything sampled a 16×16 light texture; "no textures" |
| `TextureSetParam` targeted the *fragment* slot | Targets whichever slot was bound last | Terrain got the lightmap's `LINEAR`/`CLAMP`; blurry blocks |
| `StateSetFaceCullCW` → `glFrontFace` | Means "cull back faces", i.e. `glCullFace` | Front faces culled; blocks rendered inside out |

**The lesson that generalises: a comment asserting the interface is safe to simplify is
not evidence.** `LinuxRender.cpp` said display lists "contain only tesselated-geometry
draw calls" and that `VERTEX_TYPE_COMPRESSED` was "provably never true off Xbox 360".
Both were false, and both claims survived precisely because they read as settled. Check
the callers.

## Chunk rendering: the four bugs behind "terrain renders as streaks"

Fixed together; recording them because each hid the next.

1. **Command buffers dropped all non-draw commands.** `Chunk::rebuild()` tesselates
   *chunk-local* vertices (`t->offset(-x,-y,-z)`, `Chunk.cpp:402`) and supplies the
   chunk's world position **only** via `translateToPos()` *inside* the
   `glNewList`/`glEndList` pair (`Chunk.cpp:391`); `LevelRenderer::renderChunks` issues
   no matrix ops around `CBuffCall`. Other lists put state inside too —
   `LevelRenderer.cpp:172` has `glDepthMask(false)` with 4J's own comment *"added to get
   depth mask disabled within the command buffer"*, and `ModelPart.cpp:290` sets
   depth-test/func/mask. `LinuxRender.cpp` now records a tagged `RecordedCmd` for every
   matrix and state entry point and replays them through the same public methods, so
   there is no duplicated state logic and a list still inherits the caller's camera.
   Recording rather than executing is also the correct `GL_COMPILE` semantic.
2. **`VERTEX_TYPE_COMPRESSED` was skipped, so no terrain geometry reached the GPU at
   all.** Despite `useCompactFormat360`'s name this is **the** terrain format:
   `Chunk::rebuild` calls `t->useCompactVertices(true)` unconditionally
   (`Chunk.cpp:390`). No header or shader for the layout exists in the leak, but the
   write side fully determines it — `Tesselator::vertex()`, generic branch,
   `Tesselator.cpp:824-841`. 16 bytes = 8 signed shorts: `[0..2]` position × 1024
   (chunk-local, so it fits), `[3]` RGB565 biased by −32768 so it round-trips a signed
   short, `[4]` u × 8192, `[5]` v × 8192, `[6..7]` secondary/lightmap UVs (no consumer
   here yet). `ExpandCompressedVertices()` decodes it into the 32-byte format at
   *record* time, so the cost lands once on the rebuild thread instead of every frame.
   The decode was validated by a standalone harness applying Tesselator's exact packing
   then the exact unpacking — worth redoing if you touch it, it takes five minutes and
   removes all doubt.
3. **Texture channel transposition at the source.** `LoadTextureData` uses stb, which
   returns RGBA **bytes**; the engine treats every loaded pixel as a packed **ARGB int**
   (`Texture.cpp:591`/`:643` read `>>24` as alpha, `>>16` as red), which on a
   little-endian host means bytes must be laid out B,G,R,A. Handing over stb's order
   transposed red and blue for every texture: water red, pigs blue, wood/sand/TNT blue.
   Fixed in `LoadTextureData`/`LoadTextureDataFromMemory` (and inverted in
   `SaveTextureData`) rather than by uploading as `GL_BGRA`, so the engine's own
   per-pixel colour maths — `crispBlend` mip generation, biome tinting, stitching — also
   sees the channels it thinks it has.
4. **Vertex colour byte order.** `Tesselator` packs `col = (r<<24)|(g<<16)|(b<<8)|a`
   (`Tesselator.cpp:325`) and stores that `unsigned int` straight into the vertex, so on
   little-endian the attribute arrives as a,b,g,r. The vertex shader reverses it with
   `a_colour.wzyx`. The packing is big-endian-oriented, left over from the console
   targets — the same line carries the comment *"4J - removed little-endian option"*.

Also in that pass: quads now expand through a shared index buffer and one
`glDrawElements` per batch instead of one `glDrawArrays` per quad, and `CBuffSize()` is
O(1) via a running byte total because `updateDirtyChunks` polls it up to 10× per tick
(`LevelRenderer.cpp:1807`) and summing under the store lock would stall rendering in
proportion to loaded world size.

## `C4JRender`'s command buffers are recorded from four threads at once

`LinuxRender.cpp` originally kept all renderer state in unsynchronized globals. The
engine records command buffers concurrently from three `rebuildChunkThreadProc` threads
(`MAX_CHUNK_REBUILD_THREADS`, `LevelRenderer.h:265`) plus the "Chunk update" thread
(`GameRenderer.cpp:1138`, which also rebuilds chunks itself at `LevelRenderer.cpp:2068`),
while the main thread replays via `CBuffCall`. A single global recording index meant two
threads `push_back`ing into the same `std::vector` — which is precisely what the reported
`malloc(): unaligned tcache chunk detected` coredump showed:

```
malloc_printerr → operator new → _M_realloc_append<RecordedDraw> → push_back
  → C4JRender::DrawVertices → Tesselator::end → Chunk::rebuild
  → LevelRenderer::rebuildChunkThreadProc
```

**The tell that this was by design, not incidental:** every one of those worker threads
calls `RenderManager.InitialiseContext()` on entry (`LevelRenderer.cpp:3617`,
`GameRenderer.cpp:1145`) — the vendor renderer gives each recording thread its own
context. Linux's was a no-op whose comment assumed main-thread-only.

Now: the recording cursor and buffer are `thread_local` (the hot path stays lock-free),
`CBuffEnd` commits into the store in one locked swap, and every other `CBuff*` entry
point takes `g_cbuffMutex`. Committing whole rather than clear-then-fill also removed a
flicker-to-empty window on each rebuild. Because recording no longer touches live state,
the worker threads stop racing `g_matStack`/`g_matMode` as a side effect.

## Systemic bug class: mismatched `new[]`/`delete` (not `delete[]`)

**This is the single most important thing to know before debugging any new crash in
this codebase.** MSVC's CRT allocator silently tolerates `delete ptr;` on memory
allocated with `new T[N]` (scalar delete on an array allocation — undefined behavior
per the standard). glibc's allocator does not: it corrupts heap metadata immediately,
which then surfaces later as an unrelated `malloc()`/`free()` abort
(`free(): corrupted unsorted chunks`, `malloc_printerr`, etc.) somewhere else
entirely — a classic "crash site lies about the bug site" pattern that cost
significant debugging time this session.

**Found and fixed twice already, in unrelated classes, both in the chunk-rendering
path:**
- `Minecraft.Client/TileRenderer.cpp` — `~TileRenderer()` did `delete cache;` on
  `cache = new unsigned int[32*32*32]` (constructor). Fixed to `delete[] cache;`.
- `Minecraft.Client/Chunk.cpp` — `Chunk::ReleaseThreadStorage()` did `delete tileIds;`
  on `tileIds = new unsigned char[16*16*Level::maxBuildHeight]`
  (`Chunk::CreateNewThreadStorage()`). Fixed to `delete[] tileIds;`.

**A third, related instance: `delete` of a pointer the object doesn't own.**
`Chunk::~Chunk()` did an unconditional `delete bb;`, but `bb` (an `AABB*` allocated as
`AABB::newPermanent` in `setPos`, `Chunk.cpp:106`) is not always owned:

- `Chunk::Chunk()`, the default constructor used for the static
  `LevelRenderer::permaChunk[]` array, initialised **nothing** — so `delete bb` freed
  whatever garbage was in that memory.
- `makeCopyForRebuild()` does `this->bb = source->bb` — a deliberate *alias*, because
  `rebuild()` writes recomputed bounds back through it onto the real chunk. So after any
  rebuild, destroying a `permaChunk` **double-freed** the real chunk's AABB.

Both surface as `malloc_printerr` inside `Chunk::~Chunk()` during static destruction at
process exit (`__tcf_ZN13LevelRenderer10permaChunkE` in the backtrace), and both are
intermittent — it depends what was in the uninitialised memory. Fixed with an explicit
`Chunk::ownsBB` flag plus a default constructor that initialises its pointer members.

**Generalise from this:** the `new[]`/`delete` mismatch is one instance of a broader
pattern — this codebase frequently makes shallow, aliasing copies of objects whose
destructors free members unconditionally. When a crash lands in a destructor, ask who
*owns* the pointer before assuming the delete form is wrong.

**Given it's shown up three times in adjacent code, treat it as a codebase-wide risk, not
a fluke.** If a new heap-corruption crash appears anywhere in
`Minecraft.Client`/`Minecraft.World`, before doing anything else:
1. Get the crash backtrace (`coredumpctl list` / `coredumpctl debug <pid> --debugger=gdb
   --debugger-arguments="-batch -ex 'thread apply all bt' -ex quit"`).
2. Find the containing class/function, then grep it (and anything it directly frees)
   for `new T[...]` vs. a same-class `delete` without `[]`.
3. If nothing turns up locally, consider building an AddressSanitizer variant of the
   `Minecraft.Client.Linux` CMake target (`-fsanitize=address` on both compile and
   link flags) — this catches the exact line of the invalid write/free immediately,
   which is far faster than the post-mortem gdb approach used so far. Not yet tried
   this session; worth doing before another long manual hunt.

## Real (non-Linux-specific) bugs found and fixed

These were genuine pre-existing bugs — not portability gaps — that just never
surfaced on MSVC/other platforms. Fixed as ordinary bug fixes, not with `#ifdef
_LINUX64` guards:

- `Minecraft.World/MemoryLevelStorageSource.cpp` — malformed `new` expression:
  `shared_ptr<LevelStorage> () new MemoryLevelStorage());` → `shared_ptr<LevelStorage>
  (new MemoryLevelStorage());`
- `Minecraft.World/MemoryLevelStorage.cpp`/`.h` — `ConsoleSaveFile(...)` constructed
  where `ConsoleSavePath(...)` was meant (abstract-class instantiation error); also
  missing 5 pure-virtual overrides and a signature mismatch on
  `loadPlayerDataTag`.
- `Minecraft.World/OldChunkStorage.cpp` — needed a real `_itow` implementation (the
  source had its own comment anticipating this: "need a gcc version of `_itow`?").
- `Minecraft.Client/Texture.cpp` — `int hh = height >> height;` should read
  `height >> level` (copy-paste bug next to the correct `ww = width >> level` line
  above it) — caused a zero-sized mip-level allocation.
- `Minecraft.World/Recipes.cpp` — `va_arg(vl, wchar_t)` is undefined behavior (reading
  a `va_arg` with a type other than what it was promoted/stored as); caused a SIGILL
  under GCC/x86-64. Fixed to read as `int` then narrow, the standard-conforming
  pattern.
- Several static members with out-of-line definitions guarded
  `#if __PS3__ || __ORBIS__ || __PSVITA__` only (`Tile::cloth_Id`/`glass_Id`/
  `bookshelf_Id`/`lightGem_Id` in `Tile.cpp`, `RemoveEntitiesPacket::MAX_PER_PACKET`)
  — those three console toolchains apparently enforce strict ODR on static const
  members the way GCC does; MSVC doesn't need the definition to link. Widened the
  guards to include `_LINUX64`.
- `LeaderboardManager::m_instance` — genuinely had no out-of-line definition
  anywhere in the repo. Added `= NULL`.

## Real deadlocks found in `LinuxStubs.cpp` (Linux-specific, but real concurrency bugs)

- **`InitializeCriticalSection` created a non-recursive mutex.** Win32's
  `CRITICAL_SECTION` is always reentrant — a thread re-entering a lock it already
  holds is valid and common. `pthread_mutex_init(cs, NULL)` gives a non-recursive
  mutex; any such re-entry silently deadlocked. Fixed with
  `PTHREAD_MUTEX_RECURSIVE` (see `GetRecursiveMutexAttr()`).
- **`WaitForSingleObject` assumed every `HANDLE` was a `LinuxEvent*`.** Real Win32
  code legitimately calls `WaitForSingleObject` on a *thread* handle too (waits for
  termination) — `C4JThread::WaitForCompletion()` does exactly this. But
  `CreateThread()` returns a `LinuxThread*`, a completely different struct layout.
  Reinterpreting it as a `LinuxEvent*` and locking `->mutex` at that offset read
  garbage memory as a pthread mutex, which happened to look permanently locked.
  Fixed by adding a shared `LinuxHandleKind` tag as the first field of both
  `LinuxEvent` and `LinuxThread`, and giving `LinuxThread` its own real
  completion-wait mechanism (`WaitThreadWithTimeout`, mirroring
  `WaitEventWithTimeout`'s condvar pattern).

**If a new hang/deadlock appears**, the diagnostic technique that found both of the
above: attach gdb to the live (not yet crashed) process
(`gdb -p <pid> -batch -ex "thread apply all bt"`), find which thread is blocked in
`pthread_mutex_lock`/`pthread_cond_wait` inside `LinuxStubs.cpp`, and check whether
that specific wait primitive is being used on a handle type it wasn't designed for,
or whether the mutex/cond pair backing it was ever actually signaled. Comparing two
gdb snapshots ~30-60s apart (does the blocked thread's position/CPU time change at
all?) reliably distinguishes "genuinely stuck" from "just slow."

## `ShutdownManager` was a real gap, not a rendering issue

`GameRenderer.cpp`/`MinecraftServer.cpp`/`ServerLevel.cpp`/`C4JThread.cpp`/
`Connection.cpp`/`Socket.cpp` all gate their background-thread loops on
`ShutdownManager::ShouldRun(threadId)`. A real, working reference implementation
already existed in the leak — `Minecraft.Client/PSVita/PSVitaExtras/ShutdownManager.cpp`
— but it's `#ifdef __PS3__`-gated inside a PSVita-path file (an odd pre-existing
organizational quirk), so every other platform including Windows64 silently got a
no-op stub where `ShouldRun` always returns `true`. That means those background
threads never exit on their own. Without a real implementation, the process exits
(on window close) while those threads are still running, racing static-destructor
teardown — this was the crash on window close.

Ported to Linux at `Minecraft.Client/Linux/LinuxExtras/LinuxShutdownManager.cpp`
(the header's private static members/helpers are themselves `#ifdef __PS3__`-only,
so this uses its own file-local state rather than reusing them). Wired into
`Linux_Minecraft.cpp`: `ShutdownManager::Initialise()` near the top of bootstrap,
`StartShutdown()` + `MainThreadHandleShutdown()` when the main loop exits. Two
PSVita-only calls in the reference (`StorageManager.ExitRequest`,
`ProfileManager.Terminate`) don't exist on the Windows64/Linux `4J_Storage.h`/
`4J_Profile.h` headers this port reuses, and were dropped — `LinuxStorage`'s
operations are synchronous, so there's nothing async in flight to wait for there.

## Input: movement/look read analog axes, not digital buttons

`Minecraft.Client/Input.cpp` (the shared, cross-platform per-tick input-to-player
code) reads movement and camera look via `InputManager.GetJoypadStick_LX/LY/RX/RY`
directly — **not** via `GetValue`/`ButtonDown` on the digital `_360_JOY_BUTTON_*`
bits. `LinuxInput.cpp`'s `ReadAxis()` originally returned `0.0f` unconditionally
whenever there was no physical `SDL_GameController` attached to a pad — meaning
WASD/keyboard input was fully read into digital button state correctly, but that
state was **never consulted** by the actual movement code, so keyboard movement did
nothing even though every button-state check looked correct in isolation.

Fixed: `ReadAxis()` now synthesizes a full-deflection analog value for pad 0 (no
controller) from the same digital bits `ReadPhysicalButtons()` already computes —
`LSTICK_LEFT/RIGHT` → `AXIS_MAP_LX`, `LSTICK_UP/DOWN` → `AXIS_MAP_LY`. Camera look
(`AXIS_MAP_RX/RY`) instead uses a per-frame mouse-delta value captured in `Tick()`
(`SDL_GetRelativeMouseState`, relative mouse mode engaged via
`SDL_SetRelativeMouseMode(SDL_TRUE)` in `Initialise()`), since keyboard has no
natural look-axis equivalent. Mouse left/right click was also added as an alternate
binding for `MINECRAFT_ACTION_USE`/`MINECRAFT_ACTION_ACTION` (LT/RT), alongside the
existing LSHIFT/LCTRL keyboard bindings.

**Sign convention, if this needs revisiting**: real `SDL_GameControllerAxis` reports
negative-Y = stick pushed up, positive-X = stick pushed right (same as XINPUT). The
existing (pre-this-port) digital-bit-setting code in `ReadPhysicalButtons()` already
encodes this correctly (`ly < -deadzone` → `LSTICK_UP`, `lx > deadzone` →
`LSTICK_RIGHT`). The synthesized analog fallback matches: `LSTICK_UP` → `-1.0f`,
`LSTICK_DOWN` → `+1.0f`, `LSTICK_RIGHT` → `+1.0f`, `LSTICK_LEFT` → `-1.0f`.

## The "game freezes after a while" was two bugs stacked, neither of them a hang

Reported as a mid-gameplay freeze — while moving *and* while standing still. It was
neither a deadlock nor slow rendering:

1. **Escape quit the game.** `CLinuxApp::PollEvents()` returned `false` on `SDLK_ESCAPE`,
   exiting the main loop. Escape is also mapped to the pad's pause/start button, so every
   attempt to open the pause menu silently ended the game. The log tail makes it obvious
   in hindsight — `PAUSE PRESSED` immediately followed by the shutdown banner.
2. **Shutdown then hung forever, so the window was never destroyed** and sat there
   showing its last frame — which is what made a quit *look* like a freeze. In
   `MinecraftServer::main()` the `HasStarted(eServerThread)` call was `#if __PS3__`-only
   while the matching `HasFinished` was unconditional, so the running count went `0 → -1`,
   and `WaitForSignalledToComplete()` tested `!= 0`. `-1 != 0` is true forever.

Fixed: Escape no longer quits (SHIFT+Escape does — see `BUILD_AND_RUN.md`); the
`#if __PS3__` guard widened to include `_LINUX64`; and the wait predicate changed to
`> 0` so an unbalanced count can never hang the process again.

**Also fixed there, a latent hang independent of the above:** `s_threadShouldRun` /
`s_threadRunning` are internal-linkage globals read in a spin loop whose only other
statement is `Sleep(10)`. The compiler can prove `Sleep` cannot touch internal-linkage
statics and may hoist the loads out of the loop, spinning forever regardless of what
other threads write. They are now `std::atomic`.

**Diagnostic worth reusing:** a "freeze" here is more likely a *clean exit that hung in
teardown* than a deadlock in the render loop. Check `Thread 1`'s backtrace first — if
it's in `MainThreadHandleShutdown`, the main loop already exited and the question is what
made it exit, not what is blocked. Grep the log for `PAUSE PRESSED`/shutdown banners
before attaching a debugger.

## Residual: threads that never exit (known, not fixed)

`LevelRenderer::rebuildChunkThreadProc` (`LevelRenderer.cpp:3612`) is an unconditional
`while(true)` with no `ShutdownManager::ShouldRun` check, and
`McRegionChunkStorage::runSaveThreadProc` (`McRegionChunkStorage.cpp:387`) spins in
`Sleep(1)` with no exit path. Neither is in the set `MainThreadHandleShutdown` waits on,
so neither hangs shutdown — but both are still live when `main()` returns, which is the
same static-destructor race `ShutdownManager` was added to prevent. Exit is clean and
repeatable today (verified over three runs, no coredump), so this is latent rather than
active; fixing it means adding shutdown checks to shared code.

## Ruled out (don't re-investigate these)

- **SouthPaw control-scheme swap is NOT engaged.** `Consoles_App.cpp`'s
  `eGameSetting_ControlSouthPaw` handler fully swaps `LX↔RX`/`LY↔RY` when
  `GameSettingsA[iPad]->usBitmaskValues & 0x80`. This was suspected as the cause of
  a "W and D feel swapped" report. A temporary diagnostic print confirmed
  `usBitmaskValues == 0x9600` at the check site — exactly the four bits
  `Extrax64Stubs.cpp`'s `C_4JProfile::Initialise()` explicitly sets
  (`DisplaySplitscreenGamertags|Hints|Autosave|Tooltips` = `0x0200|0x0400|0x1000|0x8000`
  = `0x9600`), with bit `0x80` clear. SouthPaw is not the cause; the real cause of
  that report is still open (see `linux-port-status.md` in `.memory/`).
- **The `GAME_SETTINGS` struct-size warning is cosmetic, not a buffer overflow.**
  `CMinecraftApp`'s constructor prints `"WARNING: The size of the profile
  GAME_SETTINGS struct has changed... Is: 208, Should be: 204"` on every boot. This
  looked like it could mean the profile-data buffer is undersized for the struct
  (classic overflow), but it isn't: the buffer is allocated at 972 bytes/player
  (`GAME_DEFINED_PROFILE_DATA_BYTES`, `Consoles_App.h`) — a completely different,
  much larger constant than the `204` used only for this sanity-check warning
  (`GAME_SETTINGS_PROFILE_DATA_BYTES`, also `Consoles_App.h`). 972 ≫ 208, so there's
  no actual overflow from this discrepancy. Harmless, pre-existing, and not worth
  chasing further.
