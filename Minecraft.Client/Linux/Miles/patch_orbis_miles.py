#!/usr/bin/env python3
"""
Turn the vendor PS4 Miles Sound System libraries into ones that link on Linux.

This is the same manoeuvre as patch_orbis_iggy.py, applied to the other RAD Game
Tools middleware in this tree. Miles exists here only as prebuilt libraries, but
Minecraft.Client/Orbis/Miles/lib/*.a is ELF x86-64 using the System V AMD64 ABI -
what Linux uses - and is not stripped, so the core is reusable as-is.

Audio turns out to be an easier case than Iggy in two ways: there is no setjmp
anywhere in these archives (so no jmp_buf size problem), and every Sony call site
lives behind a symbol we can supply ourselves, so *no object has to be dropped*.
The whole vendor library, including its RAD platform layer and its Orbis audio
driver, is kept and linked; LinuxSceShim.c provides the sce* functions underneath.

What changes:

  1. Rewrite the ELF OSABI byte from FreeBSD (9) to SYSV (0). GNU ld refuses to mix
     OSABIs; nothing else about the objects needs to change.

  2. Rewrite relocation type 40 to type 42, exactly as for Iggy. Sony's toolchain
     emits R_X86_64_PLT32_BND for GOT loads; GNU ld resolves type 40 as a plain
     PC32 to the symbol, so `rax = *(void **)&global` instead of `rax = &global`.
     R_X86_64_REX_GOTPCRELX (42) is upstream's relocation for that exact
     instruction form. Every type-40 site is verified to be a `mov r64,[rip+disp32]`
     before it is touched.

  3. Prefix every rr*/RADSS_*/g_fp_rr* symbol with `mss_`. This one is specific to
     audio and is not optional. Iggy is already linked into this client, and the two
     archives define 56 identical symbols - the whole RAD platform surface
     (rrMutex*, rrTime*, rrThread*, rrAtomic*, rrSemaphore*) plus
     RADSS_SonyInstallDriver. Iggy's copies come from LinuxRadShim.c, which
     reimplements them; Miles brings the vendor's own. Renaming Miles' set gives it
     a private, self-consistent copy: objcopy rewrites definitions and references
     together, all 56 are defined inside this archive (none is an external
     reference), and no game code refers to them - Consoles_App.h includes rrCore.h
     for its types only.

Usage:  patch_orbis_miles.py <input.a> <output.a> [work_dir]

Writes <output.a> and prints a short report. Idempotent: safe to re-run, and CMake
re-runs it whenever the input archive or this script changes.
"""

import os
import re
import shutil
import struct
import subprocess
import sys

# Per-archive expectations. These are measurements of the specific vendor libraries
# in this tree, not guesses, and the script aborts if reality disagrees - a silently
# different archive is exactly the failure this port cannot afford to debug later.
#
#   objects  : members expected in the archive
#   relocs   : R_X86_64_PLT32_BND sites expected (all of them GOT loads)
#   renames  : rr*/RADSS_*/g_fp_rr* symbols expected
PROFILES = {
    "mssorbis.a":   {"objects": 30, "relocs": 1374, "renames": 56, "interposed": 5},
    # Bink Audio: the decoder behind every .binka music file, and behind whatever
    # the soundbank stores compressed. Needs only memcpy/memmove/memset and has no
    # RAD platform layer, so nothing collides with Iggy; it still carries one GOT
    # load that has to be retargeted like the rest.
    "binkaorbis.a": {"objects": 4,  "relocs": 1,    "renames": 0,  "interposed": 0},
}

# POSIX functions the vendor code calls that we have to take over *for this archive
# only* - the same "confine an override by renaming its references" trick
# patch_orbis_iggy.py uses for setjmp. Defining these names outright would replace
# libc's for the whole client, which is not what we want.
#
# Both reasons are in rrSemaphoreCreate/DecrementOrWait (ORBIS_rrThreads.obj), and
# both are silent:
#
#   - FreeBSD's sem_t fits in 16 bytes; glibc's is 32. RAD allocates the semaphore
#     inline in the caller's buffer and puts its own "initialised" magic at +0x10 and
#     the count at +0x14, so glibc's sem_init would run straight through both, and
#     RAD's magic write would land inside glibc's semaphore state.
#   - rrSemaphoreDecrementOrWait compares errno against 0x3c after a timeout. That is
#     FreeBSD's ETIMEDOUT; Linux's is 110. The value it does not recognise falls
#     through to `ud2`, so a lock timeout aborts the process with SIGILL.
#
# Only ORBIS_rrThreads.obj references any of these, and the whole archive contains
# exactly three errno comparisons - the 0x3c above and two EINTR (4, same on both).
INTERPOSED_SYMS = {
    "sem_init":      "mss_sem_init",
    "sem_destroy":   "mss_sem_destroy",
    "sem_wait":      "mss_sem_wait",
    "sem_timedwait": "mss_sem_timedwait",
    "sem_post":      "mss_sem_post",
}

# Symbols that must not collide with the Iggy layer. Matched against every defined
# and undefined symbol in the archive; see the header comment for why this is safe.
RENAME_PATTERN = re.compile(r"^(rr|RR|RADSS_|g_fp_rr)")
RENAME_PREFIX = "mss_"

ELFOSABI_FREEBSD = 9
ELFOSABI_SYSV = 0
EI_OSABI = 7

SHT_RELA = 4
RELA_ENTSIZE = 24

# Sony's GOT-load relocation, and the upstream one meaning the same thing for a
# REX-prefixed `mov r64,[rip+disp32]`.
R_X86_64_PLT32_BND = 40
R_X86_64_REX_GOTPCRELX = 42


def elf_sections(data):
    """Yield (index, sh_type, sh_offset, sh_size, sh_info, sh_entsize) per section."""
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x3A)
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        _, sh_type, _, _, sh_offset, sh_size, _, sh_info, _, sh_entsize = \
            struct.unpack_from("<IIQQQQIIQQ", data, base)
        yield i, sh_type, sh_offset, sh_size, sh_info, sh_entsize


def is_rex_rip_load(data, insn_end):
    """True if the 3 bytes before insn_end encode `mov r64,[rip+disp32]`.

    Encoding: REX.W (0x48-0x4F), opcode 0x8B, ModRM with mod=00 and rm=101.
    Checking this rather than rewriting blindly is the point - if Sony ever used
    type 40 on a real branch, converting it to a GOT load would be catastrophic and
    silent, so we would rather stop.
    """
    if insn_end < 3:
        return False
    rex, opcode, modrm = data[insn_end - 3], data[insn_end - 2], data[insn_end - 1]
    return 0x48 <= rex <= 0x4F and opcode == 0x8B and (modrm & 0xC7) == 0x05


def retarget_got_relocs(path):
    """Rewrite R_X86_64_PLT32_BND -> R_X86_64_REX_GOTPCRELX in one object.

    Returns the number of relocations rewritten. Exits if any type-40 relocation is
    not on the expected instruction form.
    """
    with open(path, "rb") as handle:
        data = bytearray(handle.read())

    secs = list(elf_sections(data))
    by_index = {i: (off, size) for i, _t, off, size, _info, _e in secs}
    rewritten = 0

    for _i, sh_type, sh_offset, sh_size, sh_info, _ent in secs:
        if sh_type != SHT_RELA:
            continue
        target = by_index.get(sh_info)
        if target is None:
            continue
        target_offset = target[0]

        for k in range(sh_size // RELA_ENTSIZE):
            entry = sh_offset + k * RELA_ENTSIZE
            r_offset, r_info = struct.unpack_from("<QQ", data, entry)
            if (r_info & 0xFFFFFFFF) != R_X86_64_PLT32_BND:
                continue
            if not is_rex_rip_load(data, target_offset + r_offset):
                sys.exit("patch_orbis_miles: %s has a type-40 relocation at 0x%x that "
                         "is not a `mov r64,[rip]` GOT load.\n"
                         "Rewriting it as one would be wrong. Re-examine the vendor "
                         "archive before proceeding." % (os.path.basename(path), r_offset))
            new_info = (r_info & ~0xFFFFFFFF) | R_X86_64_REX_GOTPCRELX
            struct.pack_into("<Q", data, entry + 8, new_info)
            rewritten += 1

    if rewritten:
        with open(path, "wb") as handle:
            handle.write(data)
    return rewritten


def run(argv, cwd=None):
    proc = subprocess.run(argv, capture_output=True, text=True, cwd=cwd)
    if proc.returncode != 0:
        sys.exit("patch_orbis_miles: %s failed:\n%s%s"
                 % (argv[0], proc.stdout, proc.stderr))
    return proc.stdout


def tool(name):
    """Honour the standard cross-compilation environment variables."""
    return os.environ.get(name.upper(), name)


def collect_renames(work, members):
    """Every global rr*/RADSS_*/g_fp_rr* symbol in the archive, defined or referenced.

    Taken from the whole archive at once rather than per object, so a symbol defined
    in one member and referenced from another is renamed consistently on both sides.

    -g on purpose: only global symbols can collide at link time. radss_orbis.obj also
    has eleven file-local RADSS_Orbis* functions (its driver vtable), which are
    invisible to the linker and are left alone.
    """
    names = set()
    for line in run([tool("nm"), "-g", "--no-sort"] + members, cwd=work).splitlines():
        fields = line.split()
        if not fields:
            continue
        # "U name" for undefined, "<addr> T name" for defined.
        name = fields[-1]
        if len(fields) >= 2 and RENAME_PATTERN.match(name):
            names.add(name)
    return sorted(names)


def main():
    if not 3 <= len(sys.argv) <= 4:
        sys.exit(__doc__.strip())

    src = os.path.abspath(sys.argv[1])
    dst = os.path.abspath(sys.argv[2])
    work = os.path.abspath(sys.argv[3]) if len(sys.argv) == 4 else dst + ".objs"

    if not os.path.isfile(src):
        sys.exit("patch_orbis_miles: no such archive: %s" % src)

    profile = PROFILES.get(os.path.basename(src))
    if profile is None:
        sys.exit("patch_orbis_miles: no profile for %s. Add one to PROFILES with the "
                 "measured object/relocation/rename counts - the counts are what "
                 "make a substituted vendor library fail loudly instead of quietly."
                 % os.path.basename(src))

    # Always start from a clean extraction; a stale object silently linked into the
    # game would be very hard to diagnose later.
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)

    members = [m for m in run([tool("ar"), "t", src]).split("\n") if m.strip()]
    duplicates = {m for m in members if members.count(m) > 1}
    if duplicates:
        # 'ar x' would overwrite one with the other and we would lose code silently.
        sys.exit("patch_orbis_miles: archive has duplicate member names: %s"
                 % ", ".join(sorted(duplicates)))
    if len(members) != profile["objects"]:
        sys.exit("patch_orbis_miles: %s has %d objects, expected %d. The vendor "
                 "library differs from the one this port was built against."
                 % (os.path.basename(src), len(members), profile["objects"]))

    subprocess.run([tool("ar"), "x", src], cwd=work, check=True)

    renames = collect_renames(work, members)
    if len(renames) != profile["renames"]:
        sys.exit("patch_orbis_miles: found %d RAD platform symbols to rename in %s, "
                 "expected %d.\nThese are what would collide with the Iggy layer, so "
                 "an unexpected count means the collision set has changed and must be "
                 "re-checked against Minecraft.Client/Linux/Iggy/LinuxRadShim.c."
                 % (len(renames), os.path.basename(src), profile["renames"]))

    # Only interpose what this archive actually references, so a stale entry in
    # INTERPOSED_SYMS shows up as a count mismatch rather than doing nothing.
    referenced = set()
    for line in run([tool("nm"), "-u", "--no-sort"] + members, cwd=work).splitlines():
        fields = line.split()
        if fields:
            referenced.add(fields[-1])
    interposed = sorted(s for s in INTERPOSED_SYMS if s in referenced)
    if len(interposed) != profile["interposed"]:
        sys.exit("patch_orbis_miles: %s references %d of the interposed libc symbols, "
                 "expected %d (%s).\nThese are the ones whose glibc behaviour differs "
                 "from FreeBSD's in a way the vendor code cannot survive; see "
                 "INTERPOSED_SYMS."
                 % (os.path.basename(src), len(interposed), profile["interposed"],
                    ", ".join(sorted(INTERPOSED_SYMS))))

    redefine_file = os.path.join(work, "redefine-syms.txt")
    with open(redefine_file, "w") as handle:
        for name in renames:
            handle.write("%s %s%s\n" % (name, RENAME_PREFIX, name))
        for name in interposed:
            handle.write("%s %s\n" % (name, INTERPOSED_SYMS[name]))

    patched_osabi = 0
    retargeted = 0
    for member in members:
        path = os.path.join(work, member)

        with open(path, "rb") as handle:
            data = bytearray(handle.read())
        if data[:4] != b"\x7fELF":
            sys.exit("patch_orbis_miles: %s is not an ELF object" % member)
        if data[EI_OSABI] == ELFOSABI_FREEBSD:
            data[EI_OSABI] = ELFOSABI_SYSV
            patched_osabi += 1
            with open(path, "wb") as handle:
                handle.write(data)
        elif data[EI_OSABI] != ELFOSABI_SYSV:
            sys.exit("patch_orbis_miles: %s has unexpected OSABI %d"
                     % (member, data[EI_OSABI]))

        if renames or interposed:
            run([tool("objcopy"), "--redefine-syms", redefine_file, path])

        # After objcopy, so our edit is the final state of the file.
        retargeted += retarget_got_relocs(path)

    if os.path.exists(dst):
        os.remove(dst)
    # 'ar rcs' with relative member names keeps the archive reproducible.
    subprocess.run([tool("ar"), "rcs", dst] + members, cwd=work, check=True)

    print("patch_orbis_miles: %s -> %s" % (os.path.basename(src), os.path.basename(dst)))
    print("  kept all %d objects (no object needs dropping - LinuxSceShim.c supplies "
          "the sce* layer underneath them)" % len(members))
    print("  rewrote OSABI FreeBSD->SYSV on %d objects" % patched_osabi)
    print("  renamed %d RAD platform symbols to %s* (Iggy collision)"
          % (len(renames), RENAME_PREFIX))
    print("  interposed %d libc symbols (%s)"
          % (len(interposed), ", ".join(interposed) if interposed else "none"))
    print("  retargeted %d GOT loads (PLT32_BND -> REX_GOTPCRELX)" % retargeted)

    if retargeted != profile["relocs"]:
        sys.exit("patch_orbis_miles: retargeted %d GOT loads, expected %d. Every "
                 "global access in the vendor code depends on that rewrite, so a "
                 "mismatch means this is not the archive this port expects."
                 % (retargeted, profile["relocs"]))


if __name__ == "__main__":
    main()
