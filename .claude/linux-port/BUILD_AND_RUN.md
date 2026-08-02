# Building and running the Linux port

## Dependencies

- CMake ≥ 3.16, a C++14-capable GCC/Clang
- SDL2 (headers + `.so` + pkg-config `sdl2.pc`)
- GLEW (GL 3.3 core function loader — chosen over vendoring GLAD since it's a normal
  distro package)
- Mesa/OpenGL (`libGL`, `libGLX`) - and a driver that can give a **compatibility**
  profile, which Iggy's GDraw backend requires (see `IGGY.md`). Mesa does; so do the
  proprietary drivers. `Linux_App.cpp` asks for 3.3 compatibility.
- Python 3, used at build time to adapt the vendor Iggy archive
  (`Minecraft.Client/Linux/Iggy/patch_orbis_iggy.py`)
- OpenAL-soft (headers + `.so` + pkg-config `openal.pc`)

All of the above resolve via `find_package`/pkg-config fallback chains in
`Minecraft.Client/Linux/CMakeLists.txt` — install via your distro's package manager
(on Fedora: `sdl2-devel`, `glew-devel`, `mesa-libGL-devel`, `openal-soft-devel`).

## Build

```
cmake -B build -S .
cmake --build build -j$(nproc)
```

Top-level `CMakeLists.txt` hard-fails on any non-Linux/non-64-bit
`CMAKE_SYSTEM_NAME`/`CMAKE_SIZEOF_VOID_P` — this build only ever targets Linux x64.

Two targets:
- `Minecraft.World` (static lib) — the portable game/simulation logic
- `Minecraft.Client.Linux` (executable) — everything else, linked against
  `Minecraft.World` + SDL2/OpenGL/GLEW/OpenAL

## Run — working directory matters

```
cd Minecraft.Client
../build/Minecraft.Client/Linux/Minecraft.Client.Linux
```

**The binary assumes its current working directory is `Minecraft.Client/`.** Asset
paths (`Common/Media/MediaWindows64.arc`, textures, etc.) are relative and resolved
against cwd — there is no path-resolution fallback and no helpful error message if
you run it from the wrong directory. Running it from the repo root fails almost
immediately with `Failed to load archive file!` followed by an assertion abort in
`CMinecraftApp::loadStringTable()`. This isn't documented anywhere in the vcxproj
either (Windows builds set the debugger's working directory via the IDE, which
carries no meaning for a plain CLI launch) — it's just something you have to know.

Pass `--smoke-test` to run the old Phase 3-5 standalone regression check instead of
the real game (rotating test triangle + throwaway input/storage/audio self-tests,
useful for isolating whether a regression is in the renderer/input/storage/audio
subsystems themselves vs. the real game logic built on top of them).

Pass `--direct-world` to skip the frontend and launch a world immediately - the boot
path this port used for all of Phase 7. It closes the intro scenes first
(`CloseAllPlayersScenes()`), which matters: a displayed menu legitimately blocks
gameplay input via `Minecraft.cpp:2238`, so without that the world would be unplayable
underneath the intro. Use it to test gameplay without clicking through menus, or if the
UI regresses.

By default the client now boots the **real Iggy frontend** (intro, panorama, logo,
message boxes, and the authentic in-game HUD). See `.claude/linux-port/IGGY.md`.

`iggy_spike` is a separate target for testing UI assets without the game:

```
cmake --build build --target iggy_spike
# headless: parse + run the ActionScript for any set of SWFs
./build/Minecraft.Client/Linux/Iggy/iggy_spike --media Minecraft.Client \
    Minecraft.Client/Common/Media/MainMenu720.swf
# render one scene and screenshot it
./build/Minecraft.Client/Linux/Iggy/iggy_spike --media Minecraft.Client \
    --render Minecraft.Client/Common/Media/MainMenu1080.swf /tmp/shot.png
```

## Controls

Keyboard+mouse bindings live in one place: `ReadPhysicalButtons()` in
`Minecraft.Client/Linux/LinuxExtras/LinuxInput.cpp`, which maps host input onto the
`_360_JOY_BUTTON_*` bits the shared code expects. `DefineActions()`
(`Linux_Minecraft.cpp`) maps those bits to `MINECRAFT_ACTION_*`.

| Input | Action |
|---|---|
| WASD / arrows | Move (left-stick bits **only** - see `KNOWN_BUGS.md` on the D-pad) |
| Mouse | Look |
| **Left mouse** | Break (`MINECRAFT_ACTION_ACTION` → `RT`) |
| **Right mouse** | Place / use (`MINECRAFT_ACTION_USE` → `LT`) |
| LCTRL | Break (keyboard alternate) |
| SPACE / RETURN | Jump; double-tap toggles creative flight; ascend while flying |
| **SHIFT** | Sneak; hold to descend while flying |
| TAB | Inventory · Q | Crafting · ESC | Pause (does **not** quit) |
| SHIFT+ESC | Developer quit |

Two non-obvious rules when editing these:

- **Never raise `_360_JOY_BUTTON_DPAD_*` from a movement key.** The D-pad carries debug
  actions in non-final builds (fly toggle / debug overlay / spawn creeper / change skin).
- `GetJoypadStick_LY`/`RY` are forward/up-**positive**, unlike SDL and XINPUT.

The debug overlay (fps, coordinates, biome, structure locations) is
`MINECRAFT_ACTION_GAME_INFO` on the `BACK` button, currently bound to no key - add one in
`ReadPhysicalButtons()` if you want it.

## Quitting

- The window close button, or `SIGTERM`/`SIGINT` (SDL turns both into `SDL_QUIT`) —
  handy for scripted runs: `timeout 60 ../build/.../Minecraft.Client.Linux`.
- **SHIFT+ESCAPE** is a deliberate developer quit, because relative-mouse capture
  (`SDL_SetRelativeMouseMode`) makes the close button awkward to reach.
- **ESCAPE alone does not quit** — it reaches the game as the pad's pause/start
  button. It *used* to quit (`CLinuxApp::PollEvents`), which read as a mid-game
  freeze rather than a quit; see `KNOWN_BUGS.md`.

A clean exit ends with `Shutdown manager: Complete.` in the log. If you don't see
that line, the process hung during shutdown rather than exiting — check with
`ps` rather than assuming it closed.

## Debugging crashes

```
coredumpctl list --no-pager | tail    # find the PID of the crash you care about
coredumpctl debug <pid> --debugger=gdb \
  --debugger-arguments="-batch -ex 'thread apply all bt' -ex quit"
```

For hangs/deadlocks (not crashes), attach to the *live* process instead:

```
gdb -p <pid> -batch -ex "thread apply all bt"
```

Take two snapshots ~30-60s apart; a thread whose position and cumulative CPU time
(`ps -T -p <pid> -o pid,tid,pcpu,time,comm`) don't change between them is genuinely
stuck, not just doing slow real work (chunk generation on a Debug build can look
alarmingly slow if you only take one snapshot).

## Screenshots (agent-usable visual verification)

This session's environment is KDE/Wayland, where `spectacle` **can** capture just the
game window without a human in the loop:

```
spectacle -a -b -n -o /path/shot.png     # -a active window, -b background, -n no notify
```

Launch the game in the background, wait for `SetGameStarted - true` in the log, give it
~20s to build chunks, then capture. This is how the rendering fixes in `KNOWN_BUGS.md`
were verified — worth doing, because several of those bugs looked like one thing and
measured as another. (`xdotool`/`wmctrl`/`xwininfo` are absent; `import -window root`
grabs whichever window has focus, usually the terminal.)

**Screenshots are for spotting *that* something is wrong, not *what*.** Every rendering
bug in this session was misdiagnosed from the screenshot and only pinned down by
instrumenting the renderer and reading actual numbers — bound texture id and size,
vertex colour bytes, UVs, GL filter enums. Prefer one `fprintf` in
`ApplyStateAndDraw`/`TextureBind` over another round of staring at pixels.
