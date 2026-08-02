#!/usr/bin/env python3
"""
Turn the vendor PS4 Iggy static library into one that links on Linux.

Iggy's portable core exists in this tree only as prebuilt libraries, but the PS4
build (Minecraft.Client/Orbis/Iggy/lib/libiggy_orbis.a) is ELF x86-64 using the
System V AMD64 ABI - the same ABI Linux uses - and is not stripped. So the core is
reusable as-is; what has to change is only its packaging:

  1. Drop the four objects that call Sony's OS. LinuxRadShim.c replaces exactly what
     they defined. ORBIS_rrAtomics.obj is deliberately kept - it is plain x86-64 with
     no Sony dependency.

  2. Rewrite the ELF OSABI byte from FreeBSD (9) to SYSV (0). GNU ld refuses to mix
     OSABIs; nothing else about the objects needs to change.

  3. Redirect setjmp/longjmp to our own FreeBSD-layout implementations. PS4's amd64
     jmp_buf is 96 bytes and glibc's is ~200; Iggy embeds jmp_buf inside its structs,
     so glibc's setjmp would write past the field. Renaming the references (rather
     than defining setjmp ourselves) keeps the override confined to Iggy instead of
     applying to the whole binary. See linux_iggy_setjmp.S.

  4. Rewrite relocation type 40 to type 42. This one is essential and not obvious.
     Sony's toolchain emits R_X86_64_PLT32_BND (40) for every GOT load - all 2461 of
     them in this archive are `mov r64,[rip+disp32]`, not a single branch. GNU ld
     does not treat 40 as a GOT reference: it resolves it as a plain PC32 to the
     symbol, so `rax = *(void **)&global` instead of `rax = &global`. Every global
     access in Iggy then reads the variable's first 8 bytes as if they were its
     address. IggyInit segfaults immediately on `iggy_globals`, and anything that
     survived would corrupt memory in ways very hard to trace back here.

     R_X86_64_REX_GOTPCRELX (42) is upstream's relocation for exactly this
     instruction form, so the rewrite is a rename rather than a reinterpretation, and
     it lets ld relax the load to a `lea` where the symbol is local.

Usage:  patch_orbis_iggy.py <input.a> <output.a> [work_dir]

Writes <output.a> and prints a short report. Idempotent: safe to re-run, and CMake
re-runs it whenever the input archive or this script changes.
"""

import os
import shutil
import struct
import subprocess
import sys

# Objects whose bodies are Sony-specific. LinuxRadShim.c provides replacements for
# every symbol these defined that anything else in the archive still references.
DROP_OBJECTS = {
    "ORBIS_rrThreads.obj",     # scePthread* mutexes/threads
    "ORBIS_rrTime.obj",        # sceKernelGettimeofday-based clock
    "radss_orbis.obj",         # sceAudioOut* mixer backend (never installed)
    "iggy_platform_deps.obj",  # allocator/gmtime/fence hooks + gdraw_ps4_wait
}

REDEFINE_SYMS = {
    # glibc's jmp_buf is bigger than the one these objects were compiled against.
    "setjmp":  "iggy_fbsd_setjmp",
    "longjmp": "iggy_fbsd_longjmp",

    # UTF-16 marshalling. IggyUTF16 is 16-bit, but wchar_t is 32-bit on Linux and all
    # of Common/UI hands Iggy wide strings with a plain reinterpret cast (93 of them),
    # which works only because MSVC's wchar_t is also 16-bit. Renaming these lets
    # LinuxIggyUtf16.cpp take the public names and convert, instead of putting an
    # #ifdef at every call site in shared code. Iggy's own internal calls follow the
    # rename, so they reach the vendor implementations directly and are not converted
    # twice.
    "IggyLibraryCreateFromMemoryUTF16":       "iggy_vendor_IggyLibraryCreateFromMemoryUTF16",
    "IggyPlayerCreateFastName":               "iggy_vendor_IggyPlayerCreateFastName",
    "IggyPlayerCallMethodRS":                 "iggy_vendor_IggyPlayerCallMethodRS",
    "IggySetAS3ExternalFunctionCallbackUTF16":"iggy_vendor_IggySetAS3ExternalFunctionCallbackUTF16",
    "IggySetTextureSubstitutionCallbacks":    "iggy_vendor_IggySetTextureSubstitutionCallbacks",

    # The custom-draw callback is the same problem in the *read* direction, and it is
    # what makes every in-world item icon work. Iggy fills
    # IggyCustomDrawCallbackRegion::name with real UTF-16, and 12 sites across
    # Common/UI parse it with glibc's swscanf/wcscmp through a (wchar_t *) cast -
    # UIScene_HUD's "slot_%d" (the hotbar), the container/crafting/enchanting/trading
    # slot families, UIScene_SkinSelectMenu's "Character%d" and UIScene_MainMenu's
    # "Splash". None of them ever matched on Linux, so those icons silently drew
    # nothing (UIScene_HUD logged "This is not the control we are looking for" 30k
    # times a run).
    "IggySetCustomDrawCallback":              "iggy_vendor_IggySetCustomDrawCallback",
}

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
                sys.exit("patch_orbis_iggy: %s has a type-40 relocation at 0x%x that "
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


def run(argv):
    proc = subprocess.run(argv, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit("patch_orbis_iggy: %s failed:\n%s%s"
                 % (argv[0], proc.stdout, proc.stderr))
    return proc.stdout


def tool(name):
    """Honour the standard cross-compilation environment variables."""
    return os.environ.get(name.upper(), name)


def main():
    if not 3 <= len(sys.argv) <= 4:
        sys.exit(__doc__.strip())

    src = os.path.abspath(sys.argv[1])
    dst = os.path.abspath(sys.argv[2])
    work = os.path.abspath(sys.argv[3]) if len(sys.argv) == 4 else dst + ".objs"

    if not os.path.isfile(src):
        sys.exit("patch_orbis_iggy: no such archive: %s" % src)

    # Always start from a clean extraction; a stale object silently linked into the
    # game would be very hard to diagnose later.
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)

    members = [m for m in run([tool("ar"), "t", src]).split("\n") if m.strip()]
    duplicates = {m for m in members if members.count(m) > 1}
    if duplicates:
        # 'ar x' would overwrite one with the other and we would lose code silently.
        sys.exit("patch_orbis_iggy: archive has duplicate member names: %s"
                 % ", ".join(sorted(duplicates)))

    subprocess.run([tool("ar"), "x", src], cwd=work, check=True)

    missing = DROP_OBJECTS - set(members)
    if missing:
        # The drop list is tied to this exact vendor archive. If a name no longer
        # matches, the Sony-specific code would get linked in and fail confusingly at
        # link time, so stop here instead.
        sys.exit("patch_orbis_iggy: expected to drop these objects but the archive "
                 "does not contain them: %s\n"
                 "The vendor library differs from the one this port was built "
                 "against - re-check the drop list in patch_orbis_iggy.py."
                 % ", ".join(sorted(missing)))

    keep = []
    patched_osabi = 0
    retargeted = 0
    for member in members:
        path = os.path.join(work, member)
        if member in DROP_OBJECTS:
            os.remove(path)
            continue

        with open(path, "rb") as handle:
            data = bytearray(handle.read())
        if data[:4] != b"\x7fELF":
            sys.exit("patch_orbis_iggy: %s is not an ELF object" % member)
        if data[EI_OSABI] == ELFOSABI_FREEBSD:
            data[EI_OSABI] = ELFOSABI_SYSV
            patched_osabi += 1
            with open(path, "wb") as handle:
                handle.write(data)
        elif data[EI_OSABI] != ELFOSABI_SYSV:
            sys.exit("patch_orbis_iggy: %s has unexpected OSABI %d"
                     % (member, data[EI_OSABI]))

        redefine = []
        for old, new in sorted(REDEFINE_SYMS.items()):
            redefine += ["--redefine-sym", "%s=%s" % (old, new)]
        run([tool("objcopy")] + redefine + [path])

        # After objcopy, so our edit is the final state of the file.
        retargeted += retarget_got_relocs(path)

        keep.append(member)

    if os.path.exists(dst):
        os.remove(dst)
    # 'ar rcs' with relative member names keeps the archive reproducible.
    subprocess.run([tool("ar"), "rcs", dst] + keep, cwd=work, check=True)

    print("patch_orbis_iggy: %s -> %s" % (os.path.basename(src), os.path.basename(dst)))
    print("  kept %d objects, dropped %d (%s)"
          % (len(keep), len(DROP_OBJECTS), ", ".join(sorted(DROP_OBJECTS))))
    print("  rewrote OSABI FreeBSD->SYSV on %d objects" % patched_osabi)
    print("  redirected %s" % ", ".join("%s->%s" % kv for kv in sorted(REDEFINE_SYMS.items())))
    print("  retargeted %d GOT loads (PLT32_BND -> REX_GOTPCRELX)" % retargeted)
    if retargeted == 0:
        sys.exit("patch_orbis_iggy: expected thousands of type-40 GOT loads and found "
                 "none. Every Iggy global access depends on that rewrite, so this "
                 "almost certainly means the archive is not the one this port expects.")


if __name__ == "__main__":
    main()
