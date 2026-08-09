# Miles Sound System on Linux

The client has sound: real effects out of `Minecraft.msscmp`, real Bink Audio music.

Not a reimplementation. This is the same manoeuvre as `IGGY.md` on the *other* RAD
Game Tools middleware in the tree, and it turned out to be the easier of the two.
Read `IGGY.md` first — everything about relocations, OSABI and symbol renaming is
explained there and only summarised here.

## Why this works

Audio middleware here is **Miles Sound System**, from the same vendor as Iggy. 4J
never wrapped it: there is no `4J_Sound` to match `4J_Render`/`4J_Input`/`4J_Storage`,
so the entire sound engine (`Common/Audio/SoundEngine.cpp`, ~1500 lines) is source and
only ~45 `AIL_*` entry points were ever behind the wall.

`Minecraft.Client/Orbis/Miles/lib/mssorbis.a` is the same class of artefact as
`libiggy_orbis.a` — ELF x86-64, System V ABI, FreeBSD OSABI, not stripped.

| | Iggy | Miles |
|---|---|---|
| Objects | 76 | 30 (844 KB) |
| `R_X86_64_PLT32_BND` | 2461 | **1374**, all `mov r64,[rip+disp32]` |
| `setjmp`/`longjmp` | 11 objects, needed a FreeBSD-layout `.S` | **none** |
| Sony-dependent objects | 4, all dropped | 4, **all kept** |
| External symbols | 58 after drops | 70, ~41 plain libc |

The last row is the important difference. Iggy dropped its Sony objects and
reimplemented what they provided. Miles does not have to: every Sony call site is
behind a `sce*` symbol we can supply, so the vendor's own threading, timing, file IO
**and audio driver** are all kept. That avoids recovering two more caller-allocated
layouts (`rrThread` and the `_RadSoundSystem` vtable) by disassembly.

`binkaorbis.a` — Bink Audio, the codec behind all 398 `.binka` music files — comes
along for almost free: 4 objects, one GOT relocation, and `memcpy`/`memmove`/`memset`.
`fltorbis.a` (DSP filters) and `mssorbismidi.a` are not linked; nothing uses them.

## The build: `Minecraft.Client/Linux/Miles/`

| File | Role |
|---|---|
| `patch_orbis_miles.py` | rewrites the two vendor archives into ones GNU ld accepts |
| `LinuxSceShim.c` | the 29 `sce*` calls, over pthreads/POSIX/SDL2 |
| `LinuxMiles.h` | includes `mss.h` **with the PS4 ABI** — see below, this is essential |
| `miles_spike.c` | standalone harness — load a bank, list events, play one |
| `CMakeLists.txt` | `mclinux_miles` (object lib) + `miles_spike` |

## Four things that are non-obvious and load-bearing

### 1. `mss.h` has a real Linux branch, and taking it is wrong

This is the trap. Miles *was* ported to Linux, and `mss.h:3170` has an
`#elif defined(IS_LINUX)`. Compiling against it looks obviously right and produces a
**different ABI from the PS4 library we link**, in three silent ways:

- **`MAX_SPEAKERS` is 8 on PS4 and 6 on Linux.** It sizes arrays inside `FLTSTAGE`,
  `MSS_RECEIVER_LIST`, `D3DSTATE` and `DIG_DRIVER`'s `build[]`.
- **`DIG_DRIVER` gains an `S32 released` field under `IS_LINUX`**, shifting `samples`,
  `sample_status`, `n_samples` and everything after them.
- **`MDI_DRIVER` gains four more**, and `AIL_open_digital_driver` resolves to
  `RADSS_OalInstallDriver` — a driver object the PS4 archive does not contain.

`LinuxMiles.h` presents the PS4 identity to `mss.h` instead (`__RADPS4__` +
`__RADSEKRIT2__`, RAD's pre-announcement name for it, which is what actually selects
`MAX_SPEAKERS 8`). `rrCore.h` is included first and unmodified, so base types and
intrinsics stay correct for Linux/GCC; only `mss.h`'s platform selection moves.

**Verified by preprocessing `mss.h` three ways:** the wrapper's output is
byte-identical to a real `__ORBIS__` build's and differs from a plain Linux one. Worth
re-running that diff if the vendor header is ever replaced — nothing else would catch it.

`__RADPS4__` also turns on `MSS_STATIC_RIB`, which is why `Register_RIB(BinkADec)`
compiles and why Linux is grouped with the consoles at `SoundEngine.cpp:224` rather
than with Windows64, which loads its codecs as `.asi` files from a redist directory.

### 2. Miles and Iggy define the same 56 symbols

The whole RAD platform surface — `rrMutex*`, `rrTime*`, `rrThread*`, `rrAtomic*`,
`rrSemaphore*`, plus `RADSS_SonyInstallDriver` — is defined by *both* archives.
`LinuxRadShim.c` provides 21 of them for Iggy; Miles brings the vendor's own.

`patch_orbis_miles.py` prefixes Miles' copies with `mss_`. `objcopy --redefine-syms`
rewrites definitions and references together, all 56 are defined inside the archive
(none is an external reference), and no game code touches them — `Consoles_App.h`
includes `rrCore.h` for its types only. So Miles gets a private, self-consistent copy
and Iggy is untouched.

The one place the two halves meet is `AIL_open_digital_driver`, which the PS4 branch
expands to `RADSS_SonyInstallDriver(0,0)`. `LinuxMiles.h` redirects the macro to
`mss_RADSS_SonyInstallDriver` — because the *unrenamed* name is Iggy's stub, which
returns NULL for "no audio device". Calling that one opens a null driver and every
sound goes silently nowhere.

### 3. Two glibc/FreeBSD differences abort the process, and one is `ud2`

`sem_*` is called directly by `ORBIS_rrThreads.obj`, so the archive gets its own
copies via the same rename trick (`INTERPOSED_SYMS`). Both reasons are silent:

- **`sem_t` is caller-allocated and sized for FreeBSD.** `rrSemaphoreCreate` aligns a
  pointer inside the caller's buffer, calls `sem_init(storage,...)`, then stamps a
  magic at `storage+0x10` and the count at `+0x14` — so it believes a `sem_t` is at
  most 16 bytes. glibc's is 32. `mss_sem_init` keeps only a pointer in that slot and
  puts the real `sem_t` on the heap. Same hazard `rrMutexCreate` has, one struct over.
- **`ETIMEDOUT` is 60 on FreeBSD and 110 on Linux.** `rrSemaphoreDecrementOrWait`
  compares errno against `0x3c`, then against `4` (`EINTR`, which agrees), and runs
  **`ud2`** if it is neither. A timed wait that legitimately timed out killed the
  process with SIGILL about a second after the first sound played.

The whole archive contains exactly three errno comparisons, all in that function.
`sce_error()` does the same translation for the `scePthread*` return values, where
`rrMutexLockTimeout` tests `0x8002003c` and `0x80020010`.

### 4. `sceAudioOutOutput` must block

`RADSS_Thread` submits exactly one grain per call and relies on the call blocking
until the device has room — that back-pressure is the mixer's entire clock. SDL2's
queued audio is the direct equivalent; `LinuxSceShim.c` waits while more than two
grains are outstanding. Without the wait Miles spins and latency grows without bound.

Every parameter is pinned by `RADSS_OrbisOpenDriver`: user `0xff`, port type 0,
index 0, **768 sample frames per grain, 48000 Hz**, and a format selector of 1 for
stereo / 2 for 8-channel — i.e. signed 16-bit. `SoundEngine::init` must therefore ask
for `48000, 16, 2` (the Orbis settings); the generic branch asks for 44100 +
`MSS_MC_USE_SYSTEM_CONFIG` and the driver rejects it outright with *"Orbis HW output
only supports 48000, with 7.1 channel or stereo."*

## Signatures came from disassembly, not from Sony's API docs

Same method note as `IGGY.md`, and it mattered again. The documented API and what the
blob actually passes differ in ways that would corrupt memory:

- **`scePthreadCreate(ScePthread *th, attr *, entry, arg, const char *name)`** — five
  arguments, the handle by address, and a trailing name. But `Join`, `Detach`,
  `Setaffinity` and `Set/Getschedparam` all load the handle and pass it **by value**.
- **All `scePthreadMutex*` take `ScePthreadMutex *`** — the address of an 8-byte slot,
  never the handle. `rrMutexCreate` puts its own flags word at `storage+8`.
- **`scePthreadMutexattrSettype` is always called with 2** = Sony's `RECURSIVE`. A
  plain default mutex self-deadlocks.
- **`scePthreadMutexTimedlock`'s timeout is microseconds** — `rrMutexLockTimeout` does
  `imul $0x3e8` on a millisecond value immediately before the call.
- **`sceKernelUsleep` is microseconds** (`0x3e8` = 1 ms, `0x1f4` = 500 µs).
- **`_FLog(x, 1)` is log10**, not log2: its one call site multiplies the result by
  `20.0f` and substitutes `-96.0f` when `x < 1e-5f`, which is a linear-amplitude-to-dB
  conversion with a floor. Note this **contradicts the `log2` guess in
  `LinuxRadShim.c`'s `_Log`**, which Iggy never reaches and so never tested. This is
  the first measurement of that flag; if Iggy ever calls `_Log` with a non-zero flag,
  fix it there too.

## Shared-code changes this needed

Five, all additive `_LINUX64` branches or obvious corrections — and three are the
exact pattern `CLAUDE.md` warns about, a platform chain listing every console but not
Linux:

| File | What |
|---|---|
| `Consoles_SoundEngine.h` / `SoundEngine.cpp` | include `Linux/Miles/LinuxMiles.h` in place of the deleted `LinuxAudioShim.h` |
| `SoundEngine.cpp:224` | `_LINUX64` added to the `Register_RIB(BinkADec)` guard. Without it the music codec is never registered. |
| `SoundEngine.cpp` driver open | `_LINUX64` grouped with `__ORBIS__` — it was falling into the generic 44100 branch the Orbis driver rejects. **This is why the driver failed to open on the first run.** |
| `SoundEngine.cpp:68-71` | `m_szSoundPath` was `"Sound/"`, copied from Durango, and does not exist relative to the client's working directory. Now `"Durango/Sound/"`, matching Windows64. |

`LinuxAudioShim.{h,cpp}` is deleted — 443 lines of OpenAL reimplementing the `AIL_*`
surface, whose soundbank layer could only ever be a stub because the format was
assumed undecodable. It would now be a duplicate definition of every real entry point,
exactly as `LinuxIggyShim.cpp` was. **OpenAL is no longer a dependency of this port.**

## Verified

- `miles_spike` loads `Durango/Sound/Minecraft.msscmp` and enumerates **157 events**
  (`Minecraft/mob/cow`, `Minecraft/dig/stone`, `Minecraft/random/explode`, …).
- Playing one goes `pending → playing → complete` over the right duration, with
  **peak amplitude 17672–23338 / 32767** reaching PipeWire.
- The client boots with the driver open, the bank loaded, `Minecraft/CacheSounds`
  enqueued, and `music/music/creative5.binka` streaming.
- `nm` on the linked client: no unresolved `sce*`, exactly one `rrMutexCreate`
  (Iggy's) and one `mss_rrMutexCreate` (Miles').
- Clean exit still reaches `Shutdown manager: Complete.` with the extra Miles threads
  running.

## Tools

```
# list a bank's events
cd Minecraft.Client && ../build/Minecraft.Client/Linux/Miles/miles_spike \
    Durango/Sound/Minecraft.msscmp

# play one for 3 seconds
cd Minecraft.Client && ../build/Minecraft.Client/Linux/Miles/miles_spike \
    Durango/Sound/Minecraft.msscmp Minecraft/mob/cow 3
```

**`MCLINUX_MILES_AUDIO_DEBUG=1`** prints the SDL backend in use and, once a second,
grains submitted plus peak amplitude. Silence and "not wired up" are indistinguishable
by ear, and this port's history is that reasoning about which one you have produces a
confident wrong answer — read the numbers instead.

```
cd Minecraft.Client && MCLINUX_MILES_AUDIO_DEBUG=1 \
    ../build/Minecraft.Client/Linux/Minecraft.Client.Linux
```

## What's left

1. **Individual effects are unconfirmed by ear.** The engine is demonstrably decoding
   and mixing, and `miles_spike` proves a named event plays, but no one has yet
   confirmed in-game that footsteps, block breaking, mobs and UI clicks each fire at
   the right moment. That is playtesting, not engineering.
2. **3D positioning is untested.** `SoundEngine` registers a custom falloff through
   `AIL_register_falloff_function_callback` and drives `AIL_set_sample_3D_position`
   per instance; whether panning and distance attenuation behave has not been checked.
3. **Volume settings.** The audio settings screen (`UIScene_SettingsAudioMenu`) exists
   and `Options.cpp:215-226` calls `updateMusicVolume`/`updateSoundEffectVolume`, but
   the round trip has not been exercised.
4. **The DLC/mash-up audio path** (`DLCTexturePack.cpp:463-485`) calls
   `CreateStreamingWavebank`/`CreateSoundbank`, which exist only on the Xbox 360 XACT
   `SoundEngine`, not the Miles one. Pre-existing, not Linux-specific.
5. **Not redistributable.** The binary now contains vendor object code from a second
   middleware package.
