# Known bugs and gotchas

## Systemic bug class: a flag or field that is never initialised, or an initialiser Linux never reaches

**This is the highest-yield thing to check when a whole feature is silently inert.** It has
now accounted for more broken gameplay than every rendering bug combined. In each case the
code looks correct, nothing errors, and a large feature is simply dead:

| Symptom | Cause |
|---|---|
| Camera could not turn at all | `InitGameSettings()` calls `SetDefaultOptions()` only under `#if defined _WINDOWS64` / `#elif __PS3__\|__ORBIS__\|_DURANGO\|__PSVITA__`. Linux matched neither, so the whole `GAME_SETTINGS` block stayed zeroed - and `ucSensitivity == 0` makes `Input.cpp:80`'s `tx = RX * (sens/100)` permanently 0. Movement was unaffected because `xa`/`ya` are not scaled that way. |
| Jump ignored; only worked in water | `static bool s_bProfileIsFullVersion;` (`Extrax64Stubs.cpp`) was never initialised, so `IsFullVersion()` reported false, so `Minecraft.cpp:1067` built a **`TrialMode`** - which derives from `FullTutorialMode`, whose `isInputAllowed()` applies tutorial input constraints that can never lift because the Iggy UI that advances the tutorial is bypassed. |
| Mobs moved in ~1 second jumps | `GetSystemTime()` hardcoded `wMilliseconds = 0` and `SystemTimeToFileTime()` ignored the field, so `System::currentTimeMillis()` only advanced in 1000ms steps. Tick loops ran a second's worth of ticks in a burst, then stalled. |
| Heap abort at exit | `Chunk::Chunk()` (default ctor, used for the static `permaChunk[]`) initialised nothing, so `~Chunk()`'s `delete bb` freed garbage. See the `new[]`/`delete` section. |

**Phase 8 found eight more of these in one sitting**, which is why this section leads the
file. All were guards listing the consoles and/or `_WINDOWS64` with no `_LINUX64` case:

| Symptom | Guard, and what Linux therefore skipped |
|---|---|
| Skin-select menu rendered no player models; every in-game item icon missing | `UIController.cpp:1232` `setupCustomDrawGameState` — no `RenderManager.StartFrame()` and no GDraw viewport reset, so the `glOrtho` below described a pixel space that did not match the bound viewport. Needed a new `gdraw_GL_setViewport_4J()` in `gdraw_sdl.c` (GDraw's GL backend had no counterpart to `gdraw_D3D11_setViewport_4J`). |
| **Worlds were never written to disk at all** | `ConsoleSaveFileOriginal.cpp:785` — Linux fell into the `#else`, which only calls `ReleaseSaveAccess()`. `SetSaveImages()`/`SaveSaveData()` never ran; compression succeeded and nothing reported an error. Also excluded `SaveSaveDataCallback` from compilation (`:809` and the header's `:35`). |
| Iggy warned "buffer holds 3333 not 5000 chars" on every boot | `UIController.cpp:2591` gates `CHAR_SIZE = 24` on `__ORBIS__`/`_DURANGO`/`_WIN64`. The rule is really "64-bit", and Linux is 64-bit but matches none of those macros, so it got 16. |
| Controls screen showed an arbitrary controller diagram | `UIScene_ControlsMenu.cpp:15` — the `#if`/`#elif` chain has **no `#else`**, so `value[0].number` was never assigned and stack garbage went to the movie's `SetPlatform()`. `UIComponent_DebugUIMarketingGuide.cpp` does the same thing correctly by pre-seeding the value before its chain. |
| Inline `{*CONTROLLER_VK_A*}` button glyphs drawn oversized | `Consoles_App.cpp:6247` and `:6413` gate the screen-width-dependent size on `_WIN64`, which is **not** defined for Linux, so a 1280x720 window used the 1080p size (45px instead of 30, 33 instead of 22). |
| Create-world soft-locked silently with a trial texture pack | `UIScene_CreateWorldMenu.cpp:602` — no `_LINUX64`, no `#else`, so no message box was raised, but the `return` below still left `m_bIgnoreInput == true`. |
| More-Options "Disable saving" toggle did nothing | `UIScene_CreateWorldMenu.cpp:622` was `_XBOX_ONE`/`__ORBIS__`-only. |
| Leaderboards menu segfaulted on open; same NULL in-game via `StatsCounter` | Not an `#ifdef` but the same shape: `LeaderboardManager::m_instance` is defined **once per platform**, in that platform's own concrete subclass. Linux had no `Leaderboards/` directory, so `Instance()` returned NULL and no caller null-checks it. Fixed with `Linux/Leaderboards/LinuxLeaderboardManager.{h,cpp}`. **Do not "fix" a missing platform singleton by defining it as NULL in the shared file** — that turns a link error that names the culprit into a segfault. |

Two habits that catch these fast:

1. When a feature is *entirely* absent rather than wrong, look for the **initialiser**, not the
   logic. Grep the platform guard list around it - `_WINDOWS64` / `__PS3__` / `__ORBIS__` /
   `_DURANGO` / `__PSVITA__` with no `_LINUX64` is the recurring shape.
2. Where a value gates behaviour, **print the gate, not the symptom.** Every input bug this
   session was found by printing the two halves of one `if` and seeing which was false -
   never by reasoning about the feature.

## Systemic bug class: latched state that nothing can clear because the UI is bypassed

> **HISTORICAL — both workarounds described below were reverted in Phase 8.** Iggy scenes
> now build and `NavigateBack()` clears the flag as designed, so honouring it is correct;
> keeping the overrides made the camera turn and the player walk around underneath an open
> menu. The section is kept because the *diagnostic shape* recurs: when a whole feature is
> inert, look for a latch that only the (previously bypassed) UI could clear.

`UIController::NavigateToScene()` sets "a menu is displayed" **before** it builds the scene
(`UIController.cpp:1481`), and only `NavigateBack()` clears it. On this build scene
construction fails (`WARNING: Scene 1 was not created` in the log), so the flag latches on at
boot and never clears. It is set in two places, and both had to be neutralised:

- `UIController::GetMenuDisplayed()` - the game-facing query. `Minecraft.cpp:2238` guards its
  **entire in-game input block** on it (`if (screen == NULL && !ui.GetMenuDisplayed(iPad))`),
  which is where block breaking and placing live, and `Gui.cpp:175` gates the whole **HUD** on
  the same value. Overridden in `LinuxUIController` to return false.
- `C_4JInput::SetMenuDisplayed()` - `UIController::SetMenuDisplayed()` forwards `true` down to
  it, and `ReadAxis()`/`ReadTrigger()` return 0 for *every* axis while it is set, so a single
  failed navigation permanently killed both camera look and WASD movement. Now ignored in
  `LinuxInput.cpp`.

Both are correct for this build rather than workarounds: no Iggy scene ever reaches the
screen, so "a menu is displayed" can only ever be spuriously true. **If some other feature
turns out to be inert, check whether it is gated on this pair** - the HUD's absence went
unnoticed for the whole of Phase 7 because nothing reported an error.

## Keyboard must not raise the D-pad bits

`ReadPhysicalButtons()` originally raised `_360_JOY_BUTTON_DPAD_*` **as well as** the
left-stick bits for WASD/arrows. In a non-final build `Minecraft.cpp:1456-1465` repurposes the
D-pad for debug functions whenever `app.GetUseDPadForDebug()` is set - and it is, at
`Consoles_App.cpp:210`:

| D-pad bit | Key it was on | Debug action it fired |
|---|---|---|
| `DPAD_UP` | W | `MINECRAFT_ACTION_FLY_TOGGLE` |
| `DPAD_DOWN` | S | `MINECRAFT_ACTION_RENDER_DEBUG` |
| `DPAD_LEFT` | A | `MINECRAFT_ACTION_SPAWN_CREEPER` |
| `DPAD_RIGHT` | D | `MINECRAFT_ACTION_CHANGE_SKIN` |

So every movement key also fired a debug action. This is the true cause of two reports that
were misattributed to axis conventions for most of the port: the long-standing spurious
"flying is on" was **W** toggling creative flight, and "W and D feel swapped" was W toggling
flight while D changed skin. Movement keys now raise stick bits only; menu navigation still
works because `ACTION_MENU_UP` and friends are mapped to `(DPAD_x | LSTICK_x)`.

## Mouse look: the interface wants a stick position, not a mouse delta

Three separate mistakes here, worth knowing before touching `LinuxInput.cpp`:

1. **`ReadAxis()` must have no side effects.** An accumulate-and-drain-on-read design broke
   because `Minecraft.cpp:1735` also samples RX/RY every frame for an idle check - and it is a
   short-circuiting `||` chain starting with LY, so look input worked *only while a movement
   key was held* (LY != 0 short-circuited before the idle check could consume the motion).
   `Tick()` now keeps a smoothed **velocity** that any number of readers can sample harmlessly.
2. **Velocity, not per-frame delta.** `Tick()` runs per frame (~100Hz) while `Input::tick`
   reads at the 20Hz game tick, so overwriting each frame discarded ~4/5 of all mouse motion.
3. **Pre-compensate for the quadratic response.** `Input.cpp:102` applies
   `tx * abs(tx) * turnSpeed`, so passing velocity straight through felt dead when slow and
   uncontrollable when fast. `ReadAxis` returns `sqrt(velocity * GAIN)`; the square cancels and
   turn rate becomes linear in mouse speed. `degrees/sec = 1000 * velocity * MOUSE_LOOK_GAIN`,
   so `MOUSE_LOOK_GAIN * 1000` is degrees-per-pixel - the one number to tune.

Also: `GetJoypadStick_LY`/`RY` are **forward/up-positive**. `Input.cpp:39` assigns
`ya = LY` unmodified and treats positive as forward, so returning SDL's or XINPUT's raw
up-is-negative convention makes W walk backwards.

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
| `MatrixPerspective` took radians | It implements `gluPerspective`, so **degrees** | ~51° FOV instead of 70°, and underwater (`fov * 60/70` = 60) `tan(30 rad)` lands past a pole so the projection inverted and collapsed - the "viewport glitches underwater" report |

**The lesson that generalises: a comment asserting the interface is safe to simplify is
not evidence.** `LinuxRender.cpp` said display lists "contain only tesselated-geometry
draw calls" and that `VERTEX_TYPE_COMPRESSED` was "provably never true off Xbox 360".
Both were false, and both claims survived precisely because they read as settled. Check
the callers.

## Systemic bug class: Linux inheriting a branch written for Direct3D

Distinct from the "platform `#ifdef` chain with no `_LINUX64`" pattern above, and harder
to spot, because Linux does not fall into a *missing* branch - it falls into a **wrong**
one that reads as a sensible default. The guard looks complete; the `#else` just happens
to assume a D3D convention.

| Site | What the `#else` assumed | Effect on Linux |
|---|---|---|
| `glWrapper.cpp:280` `glPolygonOffset` | Depth bias is in D3D's 0..1 range, so divide by 65536 | `glPolygonOffset(-3,-3)` arrived as `-0.0000457`, far under one depth unit. **Polygon offset did nothing at all**, on every caller in the tree. |
| `LinuxRender.cpp` `MatrixPerspective` | (see the seams table above) degrees vs radians | ~51° FOV, inverted projection underwater |

**The block-breaking crack overlay z-fighting was this.**
`LevelRenderer::renderDestroyAnimation` (`LevelRenderer.cpp:2183-2247`, *not* the
identically-plausible `renderHit` at `:2164`, which is a dead stub - every caller passes
`mode == 0`) draws the `destroy_0..9` atlas tiles over the block face and relies on
`glPolygonOffset(-3,-3)` to lift them off it. With the offset divided away the decal was
exactly coplanar.

Why it *flickered* rather than being uniformly wrong is worth keeping: coplanar geometry
under `GL_LEQUAL` would resolve consistently. It fought because the two draws reach the
same surface by different arithmetic - terrain from a chunk display list of chunk-local
compressed vertices plus a recorded `translateToPos()`, the overlay tesselated in
camera-relative space via `t->offset(-xo,-yo,-zo)`. Different rounding, so the winner
varied with position and angle. **"Intermittent, depends on the angle" is the signature
of coplanar depth fighting, not of a state bug.**

Fixed by grouping `_LINUX64` with `__PS3__` - the branch selects *depth-range
convention*, not platform, and PS3's GCM is the other GL-convention backend. Confirmed
by measurement, not inference: the value reaching `glPolygonOffset` went from
`-0.0000457` to `-3`.

Two related facts found while tracing this, both true on **every** platform:

- `glEnable/glDisable(GL_POLYGON_OFFSET_FILL)` never reach `C4JRender`.
  `GL_POLYGON_OFFSET_FILL` is `0` (`stubs.h:35`) and neither `glWrapper.cpp` switch has
  a `case 0`. Polygon offset is driven *entirely* by the value setter, so `(0,0)` is the
  only way to say "off". `LinuxRender`'s `StateSetDepthSlopeAndBias` now disables the
  mode itself when both arguments are zero, instead of latching it on for the process.
- The block **selection outline** is a separate function (`renderHitOutline`,
  `:2249-2280`) and never used polygon offset - it grows the AABB by `0.002f`. It was
  never affected, either before or after.

Still open, if crack overlays on grass-topped blocks fight when viewed from straight
down: `LevelRenderer.cpp:2205-2211` has a PSVita-only `+0.01f` Y hack with the comment
*"No amount of polygon offset will push this close enough to be seen above the second
tile layer when looking straight down"*. That is a genuinely different, known-hard case.

## Systemic bug class: a resource budget sized for the smallest console

A third flavour, alongside "platform chain with no `_LINUX64` arm" and "inherited a
D3D convention". Here the `#else` is a *number*, and it is the smallest one in the
chain - so Linux, which has far more memory than any console in the list, silently
gets the most constrained configuration. The failure mode is the nastiest of the
three: no error, no crash, no log line. A feature simply switches itself off once the
budget is reached, and it never switches back on.

**Distant chunks never loading was this.** `MAX_COMMANDBUFFER_ALLOCATIONS`
(`LevelRenderer.h:46-54`) had no `_LINUX64` arm, so Linux inherited the 55 MB `#else`
- below PS3's 110 MB - while running the full `_LARGE_WORLDS` world (320-chunk
`LEVEL_MAX_WIDTH`, a 36x36 render-chunk grid). `LevelRenderer::updateDirtyChunks`
computes

```cpp
unsigned int memAlloc = RenderManager.CBuffSize(-1);              // :1807
bool onlyRebuild = ( memAlloc >= MAX_COMMANDBUFFER_ALLOCATIONS ); // :1817
```

and once `onlyRebuild` latches, any chunk that has never been compiled is skipped
unless it is within 20 blocks (`:1941-1944`). That is not just a rendering gate:
`level[p]->getChunkAt(...)` sits **inside** it (`:1975`), and `ServerLevel.cpp:438`
says why - *"don't let this actually load/create any chunks, we'll let the normal
updateDirtyChunks etc. processes do that"*. So distant chunks were never **generated**,
which is why the symptom was empty void rather than fog or missing detail.

**Two things made 55 MB even smaller than it looks on Linux.** The budget is host RAM,
not a GPU command buffer - `CBuffSize(-1)` reports `g_cbuffTotalBytes`, the port's own
recorded command payload. And terrain is expanded from `VERTEX_TYPE_COMPRESSED` before
recording (`LinuxRender.cpp` `DrawVertices`), 16 bytes per vertex becoming 32, so the
same world costs twice what the console budgets were calibrated against. 55 MB behaved
like ~27 MB console-side.

Measured, standing still at spawn - this is the shape to look for:

```
[cbuff] 0 MB / 55 MB   onlyRebuild=0
[cbuff] 25 MB / 55 MB  onlyRebuild=0
[cbuff] 55 MB / 55 MB  onlyRebuild=1     <- latches within seconds, never clears
```

Fixed with a `_LINUX64` arm at 512 MB (worth ~256 MB console-side after the 2x
expansion). It then peaks around 193 MB and settles to ~125 MB, so memory really is
released; the old 55 MB was simply below the working set of a single spawn area.
`CBuffSize` also now saturates at `INT_MAX` instead of wrapping - it returns `int` from
a `size_t` and the caller assigns to `unsigned int`, so past 2 GB a negative cast would
reappear as a huge unsigned value and latch `onlyRebuild` permanently, i.e. it would
look exactly like this bug returning.

### Consequence worth knowing: the fix costs framerate

Unlatching generation means far more terrain is loaded and drawn - measured at ~2.84 M
chunk command-list replays in 40 s, roughly 1,180 per frame, with the render thread
spending 45% of wall time in `CBuffCall`. Linux therefore also drops the default render
distance to "normal" (`Options.cpp`, `viewDistance = 1` -> 128-block far plane) and the
chunk grid to 12 chunks (`LevelRenderer.h` `PLAYER_VIEW_DISTANCE`), so it stops building
terrain that is never drawn. **The grid must stay comfortably larger than the far
plane** - if the render distance ever goes back to "far" (256 blocks = 16 chunks),
raise the grid with it or the horizon will show holes inside the fog.

## Two parallelism theories that measurement killed

Both looked convincing and both were wrong. Recorded because the *reasoning* was
plausible enough to be worth inoculating against.

- **"`veryNearCount > 0` collapses all four rebuilds onto one thread."** The code does
  do that (`LevelRenderer.cpp:2059`), but `_CRITICAL_CHUNKS` is defined
  (`LevelRenderer.h:32`), so `veryNearCount` counts only *critical* nearby chunks, not
  merely near ones. Measured: **12 atomic batches out of 9,000** (0.1%), with 26,964 of
  36,000 chunk rebuilds correctly dispatched to the worker threads.
- **"The single `g_cbuffMutex` serialises the rebuild threads against the renderer."**
  `CBuffCall` does hold the global mutex across an entire chunk replay. Measured:
  **39 ms of lock wait across 2.84 M calls.** It holds the lock ~45% of wall time, which
  throttles the workers' commits, but the render thread essentially never blocks.

### And a measurement that was itself the bug

The investigation first "found" all three rebuild threads at **0 CPU ticks**, which was
wrong. `/proc/<pid>/task/<tid>/stat` was being parsed by splitting on whitespace - and
thread names now contain spaces, so `(Rebuild Chunk T)` shifted every field by two and
`utime`/`stime` were read from `majflt`/`cmajflt`. A `gdb` backtrace settled it in one
step: two of the three threads were mid-`Tesselator::tex` at that instant. **Parse
`stat` by taking everything after the last `)`.**

Same session, same lesson twice more: a reported "performance got worse" turned out to
be the debug `fprintf`s added to diagnose it, going to an unbuffered terminal from the
render path. And a rebuild that "succeeded" had never run at all - an earlier `cd` had
left the shell in `Minecraft.Client/`, so `cmake --build build` addressed a path that
does not exist, and the output was filtered for lowercase `error`. **Check the binary
is newer than the source before trusting a test.**

## Threads are now named, and there are 37 of them

`SetThreadName` (`Minecraft.World/ThreadName.cpp`) was a Windows-only SEH trick and did
nothing on Linux, so every thread appeared as `Minecraft.Clien` in `top`/`btop`/`gdb`.
It now calls `pthread_setname_np` for `_LINUX64`, using the names the engine already
supplies - `Rebuild Chunk Thread 0`, `Chunk update`, `Server`, `Tile update`,
`McRegion Save thread 0`, `Connection #0 r/w`. The 16-byte limit means truncation, and
the `(4J) ` prefix `C4JThread` adds is dropped so it does not eat a third of the name.

For reference, since "the game only uses a few threads" is easy to believe: a running
client has **37** threads, 14 of them the game's own, the rest Mesa, SDL/PipeWire and
Miles. Thread counts here are hardcoded literals identical on every platform, and there
is no core-count query anywhere in the tree - **btop hides threads unless you press
`t`.**

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

## Systemic bug class: `memset`/`ZeroMemory` over a struct that holds a `std::string`

Same family as the `new[]`/`delete` mismatch above — UB that MSVC happens to survive and
glibc does not — but it fails *immediately and locally* rather than corrupting the heap
for later, so it is much easier to recognise once you know the shape.

`Common/UI/UIStructs.h`'s `_LaunchMoreOptionsMenuInitData()` opened with
`memset(this, 0, sizeof(*this))` as a shortcut for "default everything", then assigned the
non-zero defaults. But the struct has a `wstring seed` member. Zeroing it leaves
libstdc++'s `_M_p` as `nullptr`, so `_M_is_local()` is false and the string believes it
owns a heap buffer of capacity 0. The very next `seed = L"";` then takes the
"fits in existing capacity" path, reaches `_M_set_length(0)`, and writes the terminator
through the null pointer.

MSVC survives it because a zeroed `_Myres` still selects the small-string buffer, so the
same `memset` is harmless there. **Every screen deriving from `IUIScene_StartGame`
(`UIScene_CreateWorldMenu` and `UIScene_LoadMenu`) segfaulted in its constructor** — i.e.
create-world and load-world were both completely unreachable on Linux. Fixed by
initialising the members explicitly and leaving `seed` to its own constructor.

Swept the tree for the pattern: `UIStructs.h:268` was the **only** `memset(this, …)` over a
non-trivial type. The other 15 hits are either POD arrays (`memset(this->cache, …)`) or
console-only `PlayerUID`/`GameSessionUID` PODs, all fine. `_SaveListDetails` in the same
header does it correctly — it `ZeroMemory`s its `char[]`/`wchar_t[]` members individually
rather than the whole object.

Generalise: **any `memset`/`ZeroMemory` whose destination is `this` or a struct address is
suspect.** Check every member for a non-trivial type before trusting it, and prefer
per-member initialisation.

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
