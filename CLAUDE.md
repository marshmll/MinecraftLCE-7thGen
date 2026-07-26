# MinecraftLCE-7thGen

This is a leaked Minecraft: Legacy Console Edition (the 4J Studios-developed console
port) source tree. It natively builds only via hand-maintained `.sln`/`.vcxproj` files
targeting eight MSVC/vendor toolchains: Win32, x64, ARM64EC, Xbox 360, Xbox One
(Durango), PS3, PS4 (Orbis), PS Vita. None of those toolchains are usable outside
their original vendor SDKs.

## Active effort: a Linux x64 port

An additive CMake build for a brand-new **Linux x64** target is being built up
(never existed before — no POSIX/GL code path existed in this tree at all until this
effort started). The `.sln`/`.vcxproj` files are untouched and remain the build path
for Windows/consoles; CMake is a parallel, independent build system whose only
current target is Linux x64.

Goal (explicitly chosen by the project owner over a plain build-system port): a
**genuinely working Linux client with real OpenGL rendering**, not just a config
change — the renderer, input, and storage systems are closed-source 4J Studios
middleware with zero source in the leak, so making them work on Linux means
reimplementing them from scratch against real APIs (OpenGL/SDL2/OpenAL/POSIX).

**Status: that goal is met.** The client builds, boots straight into a generated world,
renders correctly textured terrain, and is playable — movement, mouse look, jump,
swimming, block breaking and placing, creative flight and the HUD all work, and it exits
cleanly. What remains is rendering polish (no lightmap sampling, cloud/leaf/held-item
artefacts) and the deliberately-deferred Iggy UI. See
`.memory/linux-port-plan-and-status.md` for the live list.

**Read `.claude/linux-port/` before touching this port.** It has the deep detail this
file intentionally omits:
- `.claude/linux-port/ARCHITECTURE.md` — how the port is structured, the closed-middleware
  seams (4J_Render/Input/Storage/Profile), the Iggy UI wall and how it's bypassed,
  the direct-to-gameplay boot sequence.
- `.claude/linux-port/BUILD_AND_RUN.md` — exact build/run commands and non-obvious
  requirements (**the binary must be run with `Minecraft.Client/` as the working
  directory** — relative asset paths assume it and there is no error message if you
  get this wrong, just an early crash).
- `.claude/linux-port/KNOWN_BUGS.md` — a real, systemic bug class found repeatedly in
  this codebase (mismatched `new[]`/`delete`, tolerated by MSVC's allocator, corrupts
  glibc's heap) plus every other genuine pre-existing bug fixed during the port, and
  what's still actively broken.

For the full phase-by-phase plan and its live status, see
`.memory/linux-port-plan-and-status.md` (mirrors the working plan; check this for
"what's done vs. what's next" before starting new work).

## Ground rules for this port (apply on top of general good practice)

- **Never touch `.sln`/`.vcxproj` files or existing Windows/console code paths** for
  this effort. All Linux-specific work is additive: new `_LINUX64` `#ifdef` branches
  alongside existing platform branches, new files under `Minecraft.Client/Linux/`,
  new CMake files. If a shared (non-platform-gated) file needs a genuine bug fix
  (not a portability shim), that's fine and has happened several times — see
  `KNOWN_BUGS.md` — but the fix should read as an obvious correction, not a Linux
  hack.
- **Before assuming something is a Linux-specific gap, check whether it's a real,
  pre-existing bug that just never surfaced on MSVC.** This has been the case
  repeatedly: MSVC's CRT tolerates undefined behavior (mismatched `new[]`/`delete`,
  `va_arg` type mismatches) that glibc's allocator or GCC's stricter diagnostics
  catch immediately. Fix the real bug rather than working around the symptom.
- **When a whole feature is silently inert, look for an uninitialised field or an
  initialiser Linux never reaches — not for a logic error.** This one pattern accounted
  for more broken gameplay than every rendering bug combined (dead camera, blocked jump,
  a 1-second game clock, a missing HUD). The recurring shape is a platform guard listing
  `_WINDOWS64`/`__PS3__`/`__ORBIS__`/`_DURANGO`/`__PSVITA__` with no `_LINUX64`, or a
  `static` with no initialiser. See `KNOWN_BUGS.md`'s first two sections.
- **Debug by printing the gate, not the symptom.** Every input bug in this port was found
  by printing the two halves of a single `if` and seeing which was false; reasoning from
  the code produced a confident wrong answer nearly every time. Same for rendering:
  instrument and read actual numbers (bound texture id/size, vertex bytes, UVs, GL enums)
  rather than inferring from a screenshot.
- **Follow the existing platform-shim precedent.** PS3/Orbis/PSVita each have a
  `*Stubs.h`/`*Stubs.cpp` pair mapping Win32 primitives onto native APIs
  (`Minecraft.Client/PS3/PS3Extras/Ps3Stubs.h` is the cleanest example). Linux's
  equivalents are `Minecraft.World/LinuxExtras/LinuxStubs.h/.cpp`. When a new Win32
  API turns up unimplemented, add it there in the same style — don't invent a
  different pattern.
- **Closed-middleware headers (`4J_Render.h`/`4J_Input.h`/`4J_Storage.h`/`4J_Profile.h`)
  are reused from Windows64 wherever their surface has no D3D/Win32-only types.**
  Only fork a Linux-specific copy under `Minecraft.Client/Linux/4JLibs/inc/` when a
  signature genuinely needs to change (see `4J_Render.h`'s `Initialise()` and
  `4J_Storage.h`'s Subfile API for the two real precedents) — don't fork copies
  speculatively.
- **The modern UI system (`Common/UI/*`, ~100 concrete `UIScene_*.cpp` menu screens)
  is unconditionally built on Iggy**, a closed-source RAD Game Tools vector-UI
  engine with prebuilt Windows-only binaries and no Linux equivalent. This is
  currently and deliberately bypassed (see `ARCHITECTURE.md`) — don't try to make
  real menus work without first reading why that's a separate, much larger effort.
