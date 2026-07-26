#pragma once

// LinuxAudioShim.h - a signature-compatible shim for the subset of the Miles
// Sound System (AIL_*) API that Minecraft.Client/Common/Audio/SoundEngine.cpp
// actually calls (~40 distinct symbols, enumerated via grep against that
// file). Backed by OpenAL-soft instead of a real Miles driver.
//
// IMPORTANT SCOPE NOTE: SoundEngine.cpp itself is NOT part of the Linux
// build yet - integrating it means pulling in much of Minecraft.Client's
// game/UI tree (Options, Mob, File, ...), which is out of scope for this
// phase (see Linux_Minecraft.cpp's header comment for the same reasoning
// applied to the renderer/input in earlier phases). This shim exists so
// that integration is a mechanical "compile SoundEngine.cpp for _LINUX64"
// step later, not a from-scratch audio backend.
//
// Fidelity is split in two:
//   - The digital-driver + sample/stream playback subset (AIL_startup,
//     AIL_open_digital_driver, AIL_allocate_sample_handle, AIL_start_sample,
//     AIL_set_sample_*, AIL_open_stream/start_stream/stream_status, the 3D
//     listener/position setters) is a REAL implementation against OpenAL-soft
//     sources/buffers/listener state - this is genuinely testable and is
//     smoke-tested from Linux_Minecraft.cpp.
//   - The scripted-event/soundbank layer (AIL_add_soundbank,
//     AIL_startup_event_system, AIL_enqueue_event_*, AIL_enumerate_events,
//     AIL_enumerate_sound_instances) is a STRUCTURAL STUB ONLY: real Miles
//     soundbanks are a proprietary compiled asset format (.bnk) with no
//     spec in this leak, so there is nothing to faithfully reimplement.
//     These calls succeed just enough that init/shutdown sequences don't
//     fail, but they never actually enumerate or play a scripted event.
//     Real signatures below are taken directly from the vendored Miles SDK
//     header at Minecraft.Client/Windows64/Miles/include/mss.h (present in
//     the leak), not guessed.

#include <cstdint>

// S32/U32/.../SINTa are RAD Game Tools' common base types, shared verbatim
// between the Miles (this shim) and Iggy SDKs. Consoles_App.h pulls in both
// this header (via Consoles_SoundEngine.h, included BEFORE iggy.h in
// stdafx.h's chain) and iggy.h/rrCore.h into the same translation unit on
// Linux. Including rrCore.h ourselves first - rather than just guarding on
// __RADRR_COREH__, which wouldn't help since we might be the first include -
// guarantees a single, consistent definition regardless of inclusion order:
// some of these (S64/U64/SINTa/UINTa) are declared with a different (if
// equivalent-width) builtin than plain <cstdint> typedefs use, which is a
// hard conflicting-declaration error, not just a harmless redeclaration.
#include "../../Windows64/Iggy/include/rrCore.h"
#ifndef __RADRR_COREH__
typedef int32_t S32;
typedef uint32_t U32;
typedef int16_t S16;
typedef uint16_t U16;
typedef int64_t S64;
typedef uint64_t U64;
typedef float F32;
typedef intptr_t SINTa;
typedef uintptr_t UINTa;
typedef char C8;
#endif

struct _DIG_DRIVER; typedef struct _DIG_DRIVER *HDIGDRIVER;
struct _SAMPLE; typedef struct _SAMPLE *HSAMPLE;
struct _STREAM; typedef struct _STREAM *HSTREAM;
struct SoundBank; typedef struct SoundBank *HMSOUNDBANK;
typedef void *HEVENTSYSTEM;
typedef SINTa HMSSENUM;

#define MSS_FIRST ((HMSSENUM)-1)

typedef void (*AILMIXERCB)(HDIGDRIVER dig);
typedef F32 (*AILFALLOFFCB)(HSAMPLE sample, F32 distance, F32 rolloff_factor, F32 min_dist, F32 max_dist);
typedef void AILEVENTERRORCB(S64 relevantId, char const *resource);

// Digital driver open flags/format constants actually referenced by
// SoundEngine.cpp's call sites (grep-confirmed, values from mss.h).
// Miles' x86 calling-convention macros (__stdcall on 32-bit Windows); empty
// on every 64-bit target including real Windows64, so empty here too.
#define AILCALLBACK
#define AILCALL

#define AIL_OPEN_DIGITAL_USE_SPU0 0
#define DIG_F_STEREO_MASK 2
#define DIG_F_16BITS_MASK 4
#define DIG_F_STEREO_16 (DIG_F_STEREO_MASK | DIG_F_16BITS_MASK)
#define DIG_MIXER_CHANNELS 0
#define MSS_MC_STEREO 2
#define MSS_MC_USE_SYSTEM_CONFIG 0
#define PSP2_SUBMIT_THREAD 0
#define SMP_DONE 0x0002

// Returned by AIL_enumerate_sound_instances(). Struct layout matches mss.h
// exactly (field order/types) so a real SoundEngine.cpp reading
// SoundInfo.Sample/.Status/.UserBuffer would work unmodified.
typedef struct _MILESEVENTSOUNDINFO
{
	U64 QueuedID;
	U64 InstanceID;
	U64 EventID;
	HSAMPLE Sample;
	HSTREAM Stream;
	void *UserBuffer;
	S32 UserBufferLen;
	S32 Status;
	U32 Flags;
	S32 UsedDelay;
	F32 UsedVolume;
	F32 UsedPitch;
	char const *UsedSound;
	S32 HasCompletionEvent;
} MILESEVENTSOUNDINFO;

#define MILESEVENT_SOUND_STATUS_PENDING 0x1
#define MILESEVENT_SOUND_STATUS_PLAYING 0x2
#define MILESEVENT_SOUND_STATUS_COMPLETE 0x4

#ifdef __cplusplus
extern "C" {
#endif

// --- Core driver / startup (real, OpenAL-backed) ---
char *AIL_last_error(void);
char *AIL_set_redist_directory(char const *dir);
S32 AIL_startup(void);
void AIL_shutdown(void);
HDIGDRIVER AIL_open_digital_driver(U32 frequency, S32 bits, S32 channel, U32 flags);
void AIL_close_digital_driver(HDIGDRIVER dig);
SINTa AIL_set_preference(U32 number, S32 value);
S32 AIL_platform_property(void *object, S32 property_id, void *value, S32 value_size, S32 index);
AILMIXERCB AIL_register_mix_callback(HDIGDRIVER dig, AILMIXERCB mixcb);
void AIL_set_speaker_configuration(HDIGDRIVER dig, S32 speaker_type, S32 output_type, F32 depth);
void AIL_set_event_error_callback(AILEVENTERRORCB *errorCallback);
void AIL_set_3D_rolloff_factor(HDIGDRIVER dig, F32 factor);
U32 AIL_ms_count(void);

// --- Samples (real, OpenAL-backed) ---
HSAMPLE AIL_allocate_sample_handle(HDIGDRIVER dig);
S32 AIL_init_sample(HSAMPLE s, S32 format);
void AIL_set_sample_address(HSAMPLE s, void const *start, U32 len);
void AIL_start_sample(HSAMPLE s);
void AIL_release_sample_handle(HSAMPLE s);
void AIL_set_sample_volume_levels(HSAMPLE s, F32 left, F32 right);
void AIL_set_sample_is_3D(HSAMPLE s, S32 is3D);
void AIL_set_sample_3D_distances(HSAMPLE s, F32 max_dist, F32 min_dist, F32 unused);
void AIL_set_sample_3D_position(HSAMPLE s, F32 x, F32 y, F32 z);
void AIL_set_sample_playback_rate_factor(HSAMPLE s, F32 factor);
S32 AIL_sample_playback_rate(HSAMPLE s);
void AIL_register_falloff_function_callback(HSAMPLE s, AILFALLOFFCB callback);

// --- Listener (real, OpenAL-backed) ---
void AIL_set_listener_3D_position(HDIGDRIVER dig, F32 x, F32 y, F32 z);
void AIL_set_listener_3D_orientation(HDIGDRIVER dig, F32 fx, F32 fy, F32 fz, F32 ux, F32 uy, F32 uz);

// --- Streams (real, OpenAL-backed; only WAV/raw-PCM decoding, see .cpp) ---
HSTREAM AIL_open_stream(HDIGDRIVER dig, char const *filename, S32 stream_mem);
void AIL_close_stream(HSTREAM stream);
HSAMPLE AIL_stream_sample_handle(HSTREAM stream);
void AIL_start_stream(HSTREAM stream);
void AIL_pause_stream(HSTREAM stream, S32 onoff);
S32 AIL_stream_status(HSTREAM stream);

// --- Event system / soundbanks (STRUCTURAL STUBS - see header comment) ---
HEVENTSYSTEM AIL_startup_event_system(HDIGDRIVER dig, S32 command_buf_len, char *memory_buf, S32 memory_len);
HMSOUNDBANK AIL_add_soundbank(char const *filename, char const *name);
S32 AIL_enumerate_events(HMSOUNDBANK bank, HMSSENUM *next, char const *list, char const **name);
U64 AIL_enqueue_event_by_name(char const *name);
S32 AIL_begin_event_queue_processing(void);
S32 AIL_complete_event_queue_processing(void);
S32 AIL_enumerate_sound_instances(HEVENTSYSTEM system, HMSSENUM *next, S32 statuses, char const *label_query, U64 search_for_ID, MILESEVENTSOUNDINFO *info);
S32 AIL_enqueue_event_start(void);
S32 AIL_enqueue_event_buffer(S32 *token, void *user_buffer, S32 user_buffer_len, S32 user_buffer_is_ptr);
U64 AIL_enqueue_event_end_named(S32 token, char const *event_name);
void AIL_set_variable_float(UINTa context, char const *name, F32 value);

#ifdef __cplusplus
}
#endif

// Test-tone helper for Linux_Minecraft.cpp's smoke test - not part of the
// AIL_* surface, just a convenience entry point into this shim.
bool LinuxAudioShim_PlayTestTone(HDIGDRIVER dig);
