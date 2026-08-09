#pragma once

/*
 * LinuxMiles.h - include this instead of <mss.h> on Linux.
 *
 * The Miles runtime we link is the vendor PS4 build (see patch_orbis_miles.py), so
 * the game must see mss.h laid out exactly as that build saw it. mss.h does have a
 * real IS_LINUX branch - Miles was ported to Linux - but taking it produces a
 * DIFFERENT ABI from the blob, in three ways that are all silent:
 *
 *   1. MAX_SPEAKERS is 8 on PS4 (via __RADSEKRIT2__, RAD's pre-announcement name for
 *      it) and 6 on Linux. It sizes arrays inside FLTSTAGE, MSS_RECEIVER_LIST,
 *      D3DSTATE and DIG_DRIVER's build[].
 *   2. DIG_DRIVER gains an `S32 released` field under IS_LINUX, shifting every field
 *      after it - including `samples`, `sample_status` and `n_samples`.
 *   3. MDI_DRIVER gains four more, and AIL_open_digital_driver resolves to
 *      RADSS_OalInstallDriver, a driver object the PS4 archive does not contain.
 *
 * Any of those would be read and written by both sides at different offsets, so the
 * failure would show up as corruption a long way from the cause.
 *
 * Presenting the PS4 identity to mss.h fixes all three at once. rrCore.h is included
 * first and unmodified, so the base types, calling-convention macros and intrinsics
 * are still the correct Linux/GCC ones; only mss.h's platform selection is
 * redirected. Verified by preprocessing mss.h three ways: this header's output is
 * byte-identical to a real __ORBIS__ build's, and differs from a plain Linux one.
 *
 * __RADLINUX__ is restored afterwards so anything else in the translation unit that
 * tests it - Iggy's headers are in the same client - still sees the truth.
 */

#include "../../Orbis/Miles/include/rrCore.h"

#if defined(__RADLINUX__)
   #define MCLINUX_MILES_RESTORE_RADLINUX
   #undef __RADLINUX__
#endif

#if !defined(__RADPS4__)
   #define __RADPS4__ 1
#endif
#if !defined(__RADSEKRIT2__)
   #define __RADSEKRIT2__ 1
#endif

#include "../../Orbis/Miles/include/mss.h"

#undef __RADPS4__
#undef __RADSEKRIT2__

#if defined(MCLINUX_MILES_RESTORE_RADLINUX)
   #define __RADLINUX__ 1
   #undef MCLINUX_MILES_RESTORE_RADLINUX
#endif

/*
 * One consequence of taking the PS4 branch has to be undone here.
 *
 * That branch expands AIL_open_digital_driver to install RADSS_SonyInstallDriver,
 * the vendor's own audio backend - which is what we want, since LinuxSceShim.c
 * implements the sceAudioOut* calls underneath it. But patch_orbis_miles.py renames
 * every RADSS_ and rr symbol in the archive to mss_*, so that Miles' RAD platform
 * layer cannot collide with the one LinuxRadShim.c provides for Iggy - and the
 * unrenamed RADSS_SonyInstallDriver still belongs to Iggy, where it returns NULL to
 * mean "no audio device". Calling that one would open a null driver and every sound
 * would silently go nowhere.
 *
 * So point the macro at the renamed symbol. This is the only place the two halves of
 * that rename meet, which is why it is spelled out rather than hidden in a forwarder.
 */
#ifdef __cplusplus
extern "C" {
#endif
RADSS_OPEN_FUNC AILCALL mss_RADSS_SonyInstallDriver(UINTa, UINTa);
#ifdef __cplusplus
}
#endif

#undef AIL_open_digital_driver
#define AIL_open_digital_driver(frequency, bits, channel, flags) \
   AIL_open_generic_digital_driver(frequency, bits, channel, flags, \
                                   mss_RADSS_SonyInstallDriver(0, 0))
