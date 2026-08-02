# Iggy on Linux

`ARCHITECTURE.md` used to describe Iggy as "a bigger, harder wall than
Render/Input/Storage/Profile combined … prebuilt Windows-only `.lib`/`.dll`, zero
source, zero Linux equivalent". **That was wrong on two counts**, and this document
records what is actually true, because the correction is what makes real menus
possible.

## The two facts that change everything

1. **Iggy's OpenGL renderer ships as source.** Iggy splits into a portable core and a
   per-platform renderer called GDraw, and RAD shipped every GDraw backend as source
   under `Minecraft.Client/*/Iggy/gdraw/`. `Windows64/Iggy/gdraw/` contains a complete
   OpenGL one — `gdraw_gl_shared.inl` (80 KB) plus `gdraw_gl_shaders.inl` (43 KB) —
   which no `.vcxproj` in the leak builds, and which contains no Windows API calls at
   all. Only ~200 lines of platform glue (`gdraw_wgl.c`) was Windows-specific.

2. **`Minecraft.Client/Orbis/Iggy/lib/libiggy_orbis.a` is ELF x86-64 with the System V
   ABI.** PS4 is x86-64 and FreeBSD-derived, so the PS4 build of the Iggy core is
   directly link-compatible with Linux. It is also *not stripped*: 76 objects, 2285
   named functions, with names like `swf_decode`, `swf_play`, `as3vm_abc_decode`,
   `avm2_microcode`. It is the only usable one — the Windows `.lib` is a 50 KB import
   library for a DLL, and the PS3/Vita builds are PowerPC/ARM.

So Iggy did not need reimplementing. It needed a platform layer and some glue.

**Why reimplementing was not an option worth taking.** All layout, focus navigation,
hit-testing and animation lives in the SWF assets, not in C++: 351 SWF v9 files with
9.4 MB of ActionScript 3 bytecode across 805 symbol classes and a whole `fourj.*`
component framework. `UIControl.cpp` *reads* `x`/`y`/`width`/`height` out of the movie
rather than setting them, and 164 of ~400 Iggy call sites are
`IggyPlayerCallMethodRS` — the game is a remote control, not a UI toolkit. A
from-scratch replacement means writing a Flash player with an AVM2 interpreter.

## Status

**Wired into the game and working.** The client links Iggy, the real frontend boots, and
the authentic LCE HUD renders in-game for the first time in this port.

- Booting the client brings up the intro: panorama, Minecraft logo, the autosave
  message box with real strings from the string table, focus highlighting and the
  "Ⓐ Select" button prompt. Eight scenes load with **zero** import errors
  (Intro, Panorama, ComponentLogo, SaveMessage, PressStartToPlay, ToolTips,
  TutorialPopup, HUD).
- Input round-trips: the log shows `handlePress event` and `New Stage focus Button1/2/3`,
  i.e. C++ → SWF → AS3 → C++ via `externalCallback`.
- `--direct-world` keeps the old Phase 7 shortcut: it closes the frontend
  (`CloseAllPlayersScenes()`) and launches a world directly. Terrain plus the real
  Iggy HUD - hotbar, selection highlight, crosshair, "Ⓧ Creative / Ⓨ Inventory"
  prompts. `Gui.cpp`'s `#define RENDER_HUD 0` is no longer a gap: that HUD was always
  meant to be the Iggy one.

Known-remaining, all minor: `LabelGamertag`/`PlayerName` value paths resolve as
undefined (profile UI with no profile behind it), one scene has no `SetSafeZone` AS3
method, and `IggySetFontCachingCalculationBuffer` warns its 80 KB holds 3333 not 5000
chars.

### Also verified standalone (`iggy_spike`)

- All **335** non-skin SWFs in the tree parse, initialise and tick, on every
  resolution variant. AS3 executes — 4J's own `trace()` output appears, including
  component logic like `CorrectPixelPosition X - 16.95 to 17 - Button_Y`.
- `MainMenu1080`, `PauseMenu1080` and `CreateWorldMenu1080` render through Iggy's own
  GDraw OpenGL backend with **zero GL errors**: 9-slice panels, drop shadows, sliders,
  tick-boxes, text fields, nested clips, colour transforms.
- `cmake --build build --target iggy_spike`, then:

  ```
  # headless: parse + run AS3 for any set of SWFs
  ./build/Minecraft.Client/Linux/Iggy/iggy_spike --media Minecraft.Client \
      Minecraft.Client/Common/Media/MainMenu1080.swf

  # render one scene and screenshot it
  ./build/Minecraft.Client/Linux/Iggy/iggy_spike --media Minecraft.Client \
      --render Minecraft.Client/Common/Media/MainMenu1080.swf /tmp/shot.png
  ```

`LinuxIggyShim.cpp` is gone from the build — its no-op stubs would now be duplicate
definitions of the real entry points.

## The build: `Minecraft.Client/Linux/Iggy/`

| File | Role |
|---|---|
| `patch_orbis_iggy.py` | rewrites `libiggy_orbis.a` into something GNU ld accepts |
| `LinuxRadShim.c` | the ~20 platform functions the four dropped objects provided |
| `linux_iggy_setjmp.S` | a FreeBSD-layout `setjmp`/`longjmp`, for Iggy only |
| `gdraw_sdl.c` | SDL2/GL platform glue around the vendor `gdraw_gl_shared.inl` |
| `LinuxIggyUtf16.cpp` | UTF-16 marshalling — see below, this one is essential |
| `iggy_spike.c` | standalone harness — parse, tick, render, screenshot |
| `CMakeLists.txt` | `mclinux_iggy` (object lib) + `iggy_spike` |

### `wchar_t` is 32-bit on Linux and `IggyUTF16` is 16-bit

This was the single biggest obstacle to wiring Iggy in, and it fails silently in a way
that looks like something else entirely.

`IggyUTF16` is `unsigned short`. On MSVC `wchar_t` is also 16-bit, so all of `Common/UI`
hands Iggy wide strings with a plain reinterpret cast — **93 `string16` assignments
across 26 files**, plus the library URL, fast names, AS3 callback names and
texture-substitution names. On Linux those casts pass UTF-32. Iggy reads the low half of
the first code point, hits the zero high half, and sees a one-character string.

Nothing errors at the point of failure. What you see instead is
`Attempted to import undefined library skinGraphics.swf` for a library that logged
2021 KB of successfully-loaded data seconds earlier — because it registered under a
one-character URL that no `ImportAssets2` record matches. Then every scene fails, and
`UIScene::loadMovie` dereferences the resulting NULL.

`LinuxIggyUtf16.cpp` fixes it at the boundary rather than at 93 call sites:
`patch_orbis_iggy.py` renames five vendor entry points to `iggy_vendor_*` (the same
mechanism already used for `setjmp`) and the wrappers take the public names, convert, and
forward. Shared code is untouched, and Iggy's internal calls follow the rename so they
are not double-converted. Both directions are handled — the AS3 callback trampoline is
what makes `wcscmp(call->function_name.string, L"handlePress")` match, i.e. what makes
buttons work at all.

**The same bug exists in the read direction, and it cost a second session to find.**
`IggyCustomDrawCallbackRegion::name` is real UTF-16, and **12 sites** across `Common/UI`
parse it with glibc's `swscanf`/`wcscmp` through a `(wchar_t *)` cast: `UIScene_HUD`'s
`"slot_%d"` (the hotbar), the container/crafting/enchanting/trading slot families,
`UIScene_SkinSelectMenu`'s `"Character%d"`, `UIScene_MainMenu`'s `"Splash"`. None ever
matched, so the slot id stayed `-1` and the icon was skipped - `UIScene_HUD` logged
*"This is not the control we are looking for"* **30,141 times in a 40-second run**, which
is the kind of signal worth grepping for early. Fixed by renaming
`IggySetCustomDrawCallback` too and widening `region->name` in a trampoline; the count is
now 0.

Note what made this one hard to spot: the tutorial-popup icon rendered correctly the whole
time, because `UIComponent_TutorialPopup::customDraw` draws `m_iconItem` from C++ state and
never parses the region name. Only the name-parsing consumers were dark.

`-fshort-wchar` was considered and rejected: it would match MSVC, but the tree uses
glibc's `wcscmp`/`swprintf`/`wcslen` throughout and those need 32-bit `wchar_t`.

### Four more things that are non-obvious and load-bearing

**1. 2461 relocations must be rewritten, or every global access is wrong.**
Sony's toolchain emits `R_X86_64_PLT32_BND` (type 40) for GOT loads. Every one of the
2461 in this archive sits on a `mov r64,[rip+disp32]` — not a single branch. GNU ld
does not treat type 40 as a GOT reference; it resolves it as a plain PC32 to the
symbol, so `rax = *(void **)&global` instead of `rax = &global`. `IggyInit` segfaults
immediately on `iggy_globals`, and anything that survived that would corrupt memory far
from the cause. The script rewrites them to `R_X86_64_REX_GOTPCRELX` (type 42), which
is upstream's relocation for exactly that instruction form — so it is a rename, not a
reinterpretation, and ld can then relax the load to a `lea`.

*This was the entire difference between "segfaults on the first call" and "runs".*

**2. `jmp_buf` is 96 bytes on PS4 and ~200 on glibc.** Eleven objects use
`setjmp`/`longjmp` for error unwinding, and Iggy embeds `jmp_buf` *inside its own
structs*, sized at compile time. glibc's `setjmp` would write ~104 bytes past the
field. `linux_iggy_setjmp.S` writes only within 96 bytes using FreeBSD's slot layout;
the patch script redirects the references (`setjmp` → `iggy_fbsd_setjmp`) so libc's
version is untouched for the rest of the binary. `iggy_spike` asserts both the round
trip and the absence of overrun before doing anything else.

**3. `rrMutex` is caller-allocated.** `rrMutexCreate(self, flags)` does not allocate
and does not return a handle: it aligns a pointer inside the caller's buffer, stores it
at `self + 0x78`, and puts the OS handle at that storage `+0` with a flags word at
`+8` whose bit `0x20` means "initialised". Iggy embeds these in its structs, so the
obvious `malloc`-and-return implementation corrupts memory. Recovered by disassembling
`ORBIS_rrThreads.obj`.

**4. GDraw needs a compatibility GL profile.** `gdraw_GL_CreateContext` calls
`glGetString(GL_EXTENSIONS)` (NULL under core, tripping its own assert), resolves
`GL_ARB_shader_objects` entry points, and uses `GL_INTENSITY8` / `GL_LUMINANCE4_ALPHA4`
texture formats. Compatibility is a superset of core, so `LinuxRender`'s GLSL 330 +
VAO/VBO path is unaffected.

### What the patch script does, and why each step

1. Drops `ORBIS_rrThreads.obj`, `ORBIS_rrTime.obj`, `radss_orbis.obj`,
   `iggy_platform_deps.obj` — the only four that call Sony's OS. Keeps
   `ORBIS_rrAtomics.obj`, which is plain x86-64.
2. Rewrites `EI_OSABI` from FreeBSD (9) to SYSV (0); ld refuses to mix OSABIs.
3. Redirects `setjmp`/`longjmp` (see above).
4. Rewrites the GOT relocations (see above).

After dropping those four objects, **58 external symbols remain, ~30 of which are
plain libc**. The rest are what `LinuxRadShim.c` provides.

### Semantics recovered by disassembly, not guessed

`rrCore.h` declares rr* *types* only, never these functions, so every signature came
from `objdump -dr` on the object being replaced:

- **`rrTime`'s unit is microseconds.** `rrTimePerSecond()` is literally
  `mov eax,0xf4240; ret`; `rrTimeToMicros` is the identity; `rrTimeToMillis` *rounds*
  (`lea rax,[rdi+0x1f4]` adds 500 first).
- **`iggy_default_system_alloc(ctx, size)`** takes a context first and the size
  second (`mov rdi,rsi; jmp malloc`).
- **`iggy_fence_wait_ticks` is a `U64` variable in `.bss`**, not a function.
- **`_Log(x, 0)` is the natural logarithm** — `rrlog2` multiplies its result by the
  `.rodata` constant `1.4426950408889634` (= 1/ln 2).
- **`_Sin(x, quadrant, ...)`**: 0 → `sin`, 1 → `cos`. Call sites emit the 0/1 pair
  together wherever both are needed.
- **`_Getpctype()`** returns a Dinkumware 16-bit ctype table where bit `0x20` is `_DI`
  (digit) — confirmed by the `imul ecx,r12d,0xa` / `lea r12d,[rcx+rax-0x30]` decimal
  accumulation right after the test. It is indexed by a full UTF-16 code unit, so the
  table is sized for the whole range rather than 256 entries; a literal port would be
  an out-of-bounds read on any non-ASCII character in a text field.
- **`rrThread*` is stubbed on purpose.** `nm -u` over every kept object shows
  `rrThreadCreate`/`WaitDone`/`SetPriority`/`CleanUp`/`Sleep` referenced by
  `iggy_sound.obj` and nothing else, and Iggy audio is never installed (Windows64 is
  the only platform that enables it; `Durango_UIController.cpp:57` has it commented out
  with "Iggy crashes if I have audio enabled"). The stubs complain loudly if reached.

### One genuine gap in the vendor's own GL backend

`gdraw_gl_shared.inl:1368` calls `glUniform1f` in the `GDRAW_vformat_ihud1` path, but
`gdraw_wgl.c`'s extension list omits it, so that file does not compile as shipped — on
Windows either. `ihud1` is one of 4J's custom additions and only the D3D11 backend was
kept current with them, which fits `gdraw_wgl.c` being referenced by no project file.
`gdraw_sdl.c` adds the entry, marked as such.

Separately, Mesa's `<GL/gl.h>` declares GL through 1.3 where Windows' stops at 1.1, so
`glActiveTexture` and `glCompressedTexImage2D` collide with the vendor's function
pointers; `gdraw_sdl.c` renames those two.

## Asset model: how a screen is assembled

Worth knowing before debugging a blank screen.

- Scene SWFs are **not self-contained**. They use SWF `ImportAssets2` (878 records
  across the asset set) to pull symbols from shared "skin" libraries, which must be
  registered *first* with `IggyLibraryCreateFromMemoryUTF16`. Miss that and every scene
  fails with `Attempted to import undefined library ...`.
- Libraries are registered under a **URL that differs from the filename**. Scenes
  import `platformskinHD.swf`; the file behind it is per-platform
  (`skinHDWin.swf`, `skinHDOrbis.swf`, …).
- The platform skin must load **before** the others — `loadSkin()`'s own comment says
  the normal skin requires elements from it.
- Both the SD and HD sets must be registered. A handful of 1080p scenes import the SD
  URLs (`NewUpdateMessage1080.swf` wants `platformskin.swf`,
  `HorseInventoryMenu1080.swf` wants `skinGraphics.swf`) and fail outright otherwise.
  `loadSkins()` does the same via its `eLibraryFallback_*` block.
- `skinGraphicsTooltips.swf`, `skinTooltips.swf` and their HD variants are listed by
  `loadSkins()` but **do not exist** in this tree; `loadSkin()` tolerates that.
- All text comes from the game (`app.GetString(IDS_*)` → `SetLabel`/`Init`), so an
  unpopulated movie legitimately shows its authoring placeholder — `iggy_spike`'s
  screenshots read "abel"/"abelBlac" (clipped "Label"/"LabelBlack") for that reason,
  compounded by fonts not being installed yet.

## What's left

### Solved: the frontend never cleared the depth buffer

Three symptoms turned out to be one bug: button prompts drew their letter but no disc
behind it, the autosave icon never appeared, and the intro was black (with its animation
steps firing at the correct time).

**Iggy encodes an object id in the depth buffer.** `depth_from_id()`
(`gdraw_gl_shared.inl:1269`) turns a draw id into a depth value, and the per-draw state
block enables depth testing with `GL_LESS` whenever that draw participates
(`:1479-1490`, `r->test_id` / `r->set_id`). Per-draw tracing confirmed the button-icon
quads arrive with `test_id=1 set_id=1`.

That only works if the depth buffer starts each frame cleared. **In the frontend nothing
cleared it** — in-game the world renderer does (`GameRenderer.cpp:1291`) and
`endCustomDrawGameState` clears it again (`UIController.cpp:1337`), but with no world
running, ids accumulated across frames and every depth-tested draw failed from the second
frame onward. Fixed with a `RenderManager.Clear(GL_DEPTH_BUFFER_BIT)` at the top of
`LinuxUIController::render()`.

**The frontend-versus-in-game split was the whole clue**, and it is worth remembering as a
diagnostic move: the same asset rendered correctly in one context and not the other, which
rules out the asset and points at per-frame state.

Two wrong diagnoses were recorded here before that, both worth keeping as warnings:

- *"Bitmap-filled `DefineShape`s never rasterise."* Refuted by the in-game screenshot: the
  blue X, yellow Y and RT prompts are characters 82, 73 and 101 — all `DefineShape`s with
  fill type `0x43`, the exact construction claimed to be broken. The disc that failed
  (char 87) and one that worked (char 80) are structurally identical: same tag, format,
  38x38 bitmap, same 139-byte payload, differing only in character id. The error was
  comparing two *different prompts in different contexts* and blaming the asset.
- *"The intro is fast-forwarded by `while(IggyPlayerReadyToTick())`."* Inferred from three
  `handleAnimationStep` lines being adjacent in the log — but nothing else logs between
  them, so **adjacency in a log with no timestamps says nothing about elapsed time.** The
  project owner confirmed the timing was always correct.

Also useful, from tracing: the texgen scales come out as exactly `1/38` and `1/48` for the
38x38 and 48x48 icon bitmaps, so texgen is correct; and a bitmap fill and a *gradient* fill
select the identical shader and `GDRAW_TEXTURE_normal` mode, differing only in that a
bitmap fill derives UVs through texgen (`vformat` v2c4) while a directly-placed image
carries explicit UVs (v2tc2) and bypasses texgen entirely.

### Solved: the custom-draw matrix was column-major

Item icons and player-skin previews drew at the **centre of the screen** at the right size,
and appeared mis-coloured.

`gdraw_GetObjectSpaceMatrix` (`gdraw_shared.inl:655`) takes a final `out_col_major` flag.
**The GL backend is the only caller in the tree that passes `1`** (`gdraw_gl_shared.inl:1885`);
D3D11/D3D10/D3D9, Orbis, PS3 and PSVita all pass `0`, and the shared consumer
`UIController::setupCustomDrawMatrices` is written against row-major — it reads `mat[3]`
and `mat[7]` for the translation. Under column-major those two elements are a hard `0.0f`,
so the translate collapses to exactly `(screenWidth/2, screenHeight/2)`. The scale terms
`mat[0]`/`mat[5]` are on the diagonal and layout-invariant, which is why only the position
looked wrong.

The apparent mis-colouring was the same bug: with every region collapsed onto the same
point under `GL_LEQUAL`, the last icon drawn wins the pixel, so a *different item*
overdraws the intended one.

Fixed in three places, because one is not enough:
`gdraw_GL_CalculateCustomDraw` now passes `0`; a new `gdraw_GL_BeginCustomDraw_4J` (the
twin of `gdraw_D3D11_BeginCustomDraw_4J`) does `clear_renderstate()` + a row-major matrix;
and `LinuxUIController`'s `setupCustomDraw` / `beginIggyCustomDraw4J` call it instead of the
vendor `gdraw_GL_BeginCustomDraw`, which would recompute column-major and silently undo the
fix on the cached-slot path.

### Polish
### Polish

1. **Profile-backed UI elements**: `LabelGamertag` and `PlayerName` value paths resolve
   as undefined. There is no real profile behind them (`ProfileManager` is the stub from
   `Extrax64Stubs.cpp`), so the elements the AS3 expects are never populated.
2. **One scene lacks a `SetSafeZone` AS3 method** — `UIScene::setSafeZone()` calls it
   unconditionally on every scene.
3. **Mouse pointer**: menus navigate by focus and keycode, so a pointer is not required,
   but `Linux_App.cpp` still discards `SDL_MOUSEMOTION`/`SDL_MOUSEBUTTON*` in its
   `default:` case, and relative-mouse mode stays on while a menu is up.
4. **Text entry**: `LinuxInput.cpp`'s `RequestKeyboard` still declines immediately and
   `GetText` returns empty. `UIScene_Keyboard` (the on-screen keyboard) loads, so this
   may not matter; a real text field would need `SDL_TEXTINPUT`.
5. **Deeper screens are untested.** The intro flow and the HUD are confirmed. World
   select, create-world, inventory/crafting and the pause menu all compile and their
   SWFs all load under `iggy_spike`, but they have not been driven interactively.
6. **`--direct-world` vs the frontend.** The real launch path is
   `UIScene_CreateWorldMenu` → `StartSharedLaunchFlow()`. Once that is exercised,
   `TemporaryCreateGameStart()` can go.

## Shared-code fixes this needed

All additive `_LINUX64` branches or genuine bug fixes, none of them Linux hacks. Three
are the exact pattern `CLAUDE.md` warns about — a platform `#ifdef` chain listing every
console and Windows64 but not Linux:

| File | What |
|---|---|
| `UIController.cpp` `loadSkins()` | `_LINUX64` branch for `platformSkinPath` (it had none, so the platform skin never loaded and *every* scene failed to import it). Also registers both resolution sets, and both platform-skin URLs. |
| `UIScene.cpp` `loadMovie()` | `_LINUX64` added to the resolution chain. Without it Linux fell into an `#else` that hardcoded `1080.swf`, so a 720 window asked for 1080 scenes whose HD skin imports were not registered. |
| `UIScene.cpp` `loadMovie()` | **Genuine pre-existing bug**: after logging "Failed to load iggy scene" it fell through and dereferenced the NULL it had just detected. `FatalLoadError()` is empty on *every* platform and `__debugbreak()` only traps under a debugger, so this crashed anywhere outside a debugger. Now returns. |
| `UIScene_Intro.cpp` | `platformIdx` was undeclared on Linux — the `#ifdef` chain has no `#else`. Uses Windows64's index 0 (PC). |
| `UIController.h` | `eLibraryFallback_*` enumerators were `_WINDOWS64`-only. |
| `UIScene_MainMenu/PauseMenu/SkinSelectMenu.cpp` | 6 sites: `#if !(defined(_XBOX) \|\| defined(_WIN64))` guards a console online-service check whose `IDS_ONLINE_SERVICE_TITLE` exists only in the PS3/Orbis/Vita string tables. Linux excluded alongside `_WIN64`. |
| `UIControl_EnchantmentButton.cpp` | missing `#include <iterator>` for `std::istream_iterator` (MSVC's `<sstream>` pulls it in transitively). |
| `LinuxInput.cpp` | `SetMenuDisplayed()` restored to real behaviour; its no-op rationale ("no Iggy scene ever reaches the screen here") is no longer true. |

`UIControl_Touch.cpp` and `UIScene_InGameSaveManagementMenu.cpp` are excluded from the
Linux build — they are exactly the two `Common/UI` files `Minecraft.Client.vcxproj`
excludes from its x64 configurations.

### Risks still open

- **GL state conflict.** GDraw and `LinuxRender` share one context. `LinuxRender`
  re-uploads uniforms per draw but caches `glEnable`/`glBlendFunc`-style state in
  globals, so those need invalidating after `ui.render()`. `ui.render()` is a separate
  top-level step, never nested in world/HUD rendering, so one reset point suffices;
  custom-draw regions are the nested case and `gdraw_GL_BeginCustomDraw`/`EndCustomDraw`
  exist for exactly that handoff.
- **53 uncaught AS3 exceptions** across the full asset sweep
  (`IllegalOperationError`, `TypeError getting field $length`). Unknown whether these
  are downstream of missing fonts and no game-supplied data, or real. Re-measure after
  fonts are installed before investigating.
- **Not redistributable.** The binary now contains vendor object code.

## Seeing GL errors at all: `MCLINUX_GL_DEBUG=1`

GDraw's own error checking is **unusable in this build, and its silence means nothing.**
`opengl_check()` is wrapped in `#ifdef _DEBUG`, and `_DEBUG` is deliberately not defined
for the `mclinux_iggy` target, because the `_DEBUG` path runs `break_on_err()` →
`RR_BREAK()`, which on x86-64 is `int $3` — the first GL error anywhere in GDraw would
abort the whole client instead of reporting itself.

The consequence bit an entire debugging session: GDraw silently swallows every GL error, so
a whole class of draws can go missing with a perfectly clean log, and a clean log looks
like evidence when it is nothing of the kind.

`gdraw_sdl.c` therefore installs a KHR_debug callback when `MCLINUX_GL_DEBUG=1` is set in
the environment. It reports the same information without the breakpoint. **Set it before
concluding anything from an absence of errors.**

```
cd Minecraft.Client && MCLINUX_GL_DEBUG=1 ../build/Minecraft.Client/Linux/Minecraft.Client.Linux
```

## Method note

Every one of the four load-bearing facts above came from disassembling the vendor
object being replaced and reading actual bytes — relocation type numbers, struct
offsets, `.rodata` constants, which register held the size. Reasoning from plausible
C signatures produced a confidently wrong answer every time: the first `rrMutexCreate`
attempt allocated and returned a handle, and the first `iggy_default_system_alloc` had
the arguments the wrong way round. Both would have corrupted memory silently rather
than failing.

This is the same lesson `.memory/linux-port-plan-and-status.md` records for the
renderer and input work — print the gate, read the numbers.
