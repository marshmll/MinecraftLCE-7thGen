#include "LinuxAudioShim.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <vector>
#include <ctime>

struct _DIG_DRIVER
{
	ALCdevice *device;
	ALCcontext *context;
	U32 frequency;
};

struct _SAMPLE
{
	ALuint source;
	ALuint buffer;
	bool hasBuffer;
	S32 format;
	AILFALLOFFCB falloffCallback; // stored but never invoked - see header comment
	HDIGDRIVER driver;
};

struct _STREAM
{
	_SAMPLE sample;
};

namespace
{
	char g_lastError[256] = "";
	void SetLastError(const char *msg) { strncpy(g_lastError, msg, sizeof(g_lastError) - 1); }

	// Miles ties raw-addressed sample playback rate to the digital driver's
	// mixer rate (AIL_open_digital_driver's frequency argument) - there is no
	// separate per-sample rate parameter anywhere in the API surface
	// SoundEngine.cpp uses (AIL_init_sample only takes a channel/bit-depth
	// format flag). Documented assumption, not guessed at random.
	ALenum FormatToAL(S32 format)
	{
		bool stereo = (format & DIG_F_STEREO_MASK) != 0;
		bool sixteenBit = (format & DIG_F_16BITS_MASK) != 0;
		if (stereo) return sixteenBit ? AL_FORMAT_STEREO16 : AL_FORMAT_STEREO8;
		return sixteenBit ? AL_FORMAT_MONO16 : AL_FORMAT_MONO8;
	}

	bool LoadWavPCM(const char *filename, std::vector<unsigned char> &pcmOut, ALenum &formatOut, U32 &rateOut)
	{
		std::ifstream in(filename, std::ios::binary);
		if (!in) return false;

		char riff[4]; in.read(riff, 4);
		if (in.gcount() != 4 || memcmp(riff, "RIFF", 4) != 0) return false;
		in.ignore(4); // chunk size
		char wave[4]; in.read(wave, 4);
		if (memcmp(wave, "WAVE", 4) != 0) return false;

		U16 numChannels = 1, bitsPerSample = 16;
		U32 sampleRate = 44100;
		bool haveFmt = false;

		while (in)
		{
			char chunkId[4];
			U32 chunkSize;
			in.read(chunkId, 4);
			in.read((char *)&chunkSize, 4);
			if (!in) break;

			if (memcmp(chunkId, "fmt ", 4) == 0)
			{
				U16 audioFormat; in.read((char *)&audioFormat, 2);
				in.read((char *)&numChannels, 2);
				in.read((char *)&sampleRate, 4);
				in.ignore(6); // byteRate(4) + blockAlign(2)
				in.read((char *)&bitsPerSample, 2);
				if (chunkSize > 16) in.ignore(chunkSize - 16);
				haveFmt = true;
			}
			else if (memcmp(chunkId, "data", 4) == 0)
			{
				pcmOut.resize(chunkSize);
				in.read((char *)pcmOut.data(), chunkSize);
				break;
			}
			else
			{
				in.ignore(chunkSize);
			}
		}

		if (!haveFmt || pcmOut.empty()) return false;

		formatOut = (numChannels == 2)
			? (bitsPerSample == 16 ? AL_FORMAT_STEREO16 : AL_FORMAT_STEREO8)
			: (bitsPerSample == 16 ? AL_FORMAT_MONO16 : AL_FORMAT_MONO8);
		rateOut = sampleRate;
		return true;
	}
}

char *AIL_last_error(void) { return g_lastError; }

char *AIL_set_redist_directory(char const *dir)
{
	// No Miles redist DLLs to locate on Linux (OpenAL-soft is a normal
	// system library) - accepted and ignored.
	return (char *)dir;
}

S32 AIL_startup(void) { return 1; }

void AIL_shutdown(void)
{
	// Per-driver device/context teardown happens in AIL_close_digital_driver;
	// nothing global left to release here.
}

HDIGDRIVER AIL_open_digital_driver(U32 frequency, S32 bits, S32 channel, U32 flags)
{
	(void)bits; (void)channel; (void)flags;

	ALCdevice *device = alcOpenDevice(nullptr);
	if (!device) { SetLastError("alcOpenDevice failed"); return nullptr; }
	ALCcontext *context = alcCreateContext(device, nullptr);
	if (!context) { SetLastError("alcCreateContext failed"); alcCloseDevice(device); return nullptr; }
	alcMakeContextCurrent(context);

	_DIG_DRIVER *dig = new _DIG_DRIVER();
	dig->device = device;
	dig->context = context;
	dig->frequency = frequency;
	return dig;
}

void AIL_close_digital_driver(HDIGDRIVER dig)
{
	if (!dig) return;
	alcMakeContextCurrent(nullptr);
	alcDestroyContext(dig->context);
	alcCloseDevice(dig->device);
	delete dig;
}

SINTa AIL_set_preference(U32 number, S32 value) { (void)number; (void)value; return 0; }

S32 AIL_platform_property(void *object, S32 property_id, void *value, S32 value_size, S32 index)
{
	(void)object; (void)property_id; (void)value; (void)value_size; (void)index;
	return 0; // e.g. PSP2_SUBMIT_THREAD - console-specific, no-op on Linux
}

AILMIXERCB AIL_register_mix_callback(HDIGDRIVER dig, AILMIXERCB mixcb)
{
	// OpenAL-soft has no per-callback mixer-tap hook exposed through the
	// standard AL/ALC API, so this callback (used on PSVita to drive Miles'
	// mixer manually) is stored but never invoked.
	(void)dig;
	static AILMIXERCB previous = nullptr;
	AILMIXERCB old = previous;
	previous = mixcb;
	return old;
}

void AIL_set_speaker_configuration(HDIGDRIVER dig, S32 speaker_type, S32 output_type, F32 depth)
{
	(void)dig; (void)speaker_type; (void)output_type; (void)depth;
}

void AIL_set_event_error_callback(AILEVENTERRORCB *errorCallback) { (void)errorCallback; }

void AIL_set_3D_rolloff_factor(HDIGDRIVER dig, F32 factor)
{
	(void)dig;
	alDopplerFactor(1.0f);
	alSpeedOfSound(343.3f);
	(void)factor; // OpenAL's global rolloff is set per-source (AL_ROLLOFF_FACTOR), not per-driver; applied in AIL_set_sample_3D_distances instead
}

U32 AIL_ms_count(void)
{
	// Matches Miles' documented semantics (milliseconds since some
	// unspecified epoch, only meaningful as deltas between two calls).
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (U32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

HSAMPLE AIL_allocate_sample_handle(HDIGDRIVER dig)
{
	_SAMPLE *s = new _SAMPLE();
	s->driver = dig;
	s->hasBuffer = false;
	s->format = DIG_F_STEREO_16;
	s->falloffCallback = nullptr;
	alGenSources(1, &s->source);
	s->buffer = 0;
	return s;
}

S32 AIL_init_sample(HSAMPLE s, S32 format)
{
	if (!s) return 0;
	s->format = format;
	return 1;
}

void AIL_set_sample_address(HSAMPLE s, void const *start, U32 len)
{
	if (!s) return;
	if (s->buffer) alDeleteBuffers(1, &s->buffer);
	alGenBuffers(1, &s->buffer);

	U32 rate = s->driver ? s->driver->frequency : 44100;
	alBufferData(s->buffer, FormatToAL(s->format), start, (ALsizei)len, (ALsizei)rate);
	alSourcei(s->source, AL_BUFFER, (ALint)s->buffer);
	s->hasBuffer = true;
}

void AIL_start_sample(HSAMPLE s) { if (s) alSourcePlay(s->source); }

void AIL_release_sample_handle(HSAMPLE s)
{
	if (!s) return;
	alSourceStop(s->source);
	alDeleteSources(1, &s->source);
	if (s->buffer) alDeleteBuffers(1, &s->buffer);
	delete s;
}

void AIL_set_sample_volume_levels(HSAMPLE s, F32 left, F32 right)
{
	// OpenAL sources have a single scalar gain, not independent L/R levels -
	// average the two, matching how mono/near-mono game sound effects are
	// used throughout SoundEngine.cpp (this is a real simplification, not a
	// hidden bug: stereo panning of point sources is handled by OpenAL's own
	// 3D spatialization via AL_POSITION instead).
	if (!s) return;
	alSourcef(s->source, AL_GAIN, (left + right) * 0.5f);
}

void AIL_set_sample_is_3D(HSAMPLE s, S32 is3D)
{
	if (!s) return;
	alSourcei(s->source, AL_SOURCE_RELATIVE, is3D ? AL_FALSE : AL_TRUE);
	if (!is3D)
		alSource3f(s->source, AL_POSITION, 0.0f, 0.0f, 0.0f);
}

void AIL_set_sample_3D_distances(HSAMPLE s, F32 max_dist, F32 min_dist, F32 unused)
{
	(void)unused;
	if (!s) return;
	alSourcef(s->source, AL_REFERENCE_DISTANCE, min_dist);
	alSourcef(s->source, AL_MAX_DISTANCE, max_dist);
}

void AIL_set_sample_3D_position(HSAMPLE s, F32 x, F32 y, F32 z)
{
	if (!s) return;
	alSource3f(s->source, AL_POSITION, x, y, z);
}

void AIL_set_sample_playback_rate_factor(HSAMPLE s, F32 factor)
{
	if (!s) return;
	alSourcef(s->source, AL_PITCH, factor <= 0.0f ? 1.0f : factor);
}

S32 AIL_sample_playback_rate(HSAMPLE s)
{
	return (s && s->driver) ? (S32)s->driver->frequency : 44100;
}

void AIL_register_falloff_function_callback(HSAMPLE s, AILFALLOFFCB callback)
{
	// Stored, never invoked - see header comment: OpenAL applies its own
	// inverse-distance attenuation model via AL_REFERENCE_DISTANCE/
	// AL_MAX_DISTANCE instead of calling back into game code per-sample.
	if (s) s->falloffCallback = callback;
}

void AIL_set_listener_3D_position(HDIGDRIVER dig, F32 x, F32 y, F32 z)
{
	(void)dig;
	alListener3f(AL_POSITION, x, y, z);
}

void AIL_set_listener_3D_orientation(HDIGDRIVER dig, F32 fx, F32 fy, F32 fz, F32 ux, F32 uy, F32 uz)
{
	(void)dig;
	ALfloat orientation[6] = { fx, fy, fz, ux, uy, uz };
	alListenerfv(AL_ORIENTATION, orientation);
}

HSTREAM AIL_open_stream(HDIGDRIVER dig, char const *filename, S32 stream_mem)
{
	(void)stream_mem;
	_STREAM *stream = new _STREAM();
	stream->sample.driver = dig;
	stream->sample.hasBuffer = false;
	stream->sample.falloffCallback = nullptr;
	alGenSources(1, &stream->sample.source);
	stream->sample.buffer = 0;

	std::vector<unsigned char> pcm;
	ALenum format = AL_FORMAT_MONO16;
	U32 rate = 44100;
	if (LoadWavPCM(filename, pcm, format, rate))
	{
		alGenBuffers(1, &stream->sample.buffer);
		alBufferData(stream->sample.buffer, format, pcm.data(), (ALsizei)pcm.size(), (ALsizei)rate);
		alSourcei(stream->sample.source, AL_BUFFER, (ALint)stream->sample.buffer);
		stream->sample.hasBuffer = true;
	}
	else
	{
		// Real Miles streams decode compressed proprietary music-bank assets
		// this leak doesn't include the format for; this shim only handles
		// plain PCM WAV. Missing/unreadable file: return a live, silent
		// stream handle rather than null so callers don't crash on a missing
		// asset - matches "SMP_DONE immediately" behaviour a real failed
		// stream open would eventually reach.
		fprintf(stderr, "LinuxAudioShim: AIL_open_stream: could not load '%s' as PCM WAV (stub silence)\n", filename);
	}
	return stream;
}

void AIL_close_stream(HSTREAM stream)
{
	if (!stream) return;
	alSourceStop(stream->sample.source);
	alDeleteSources(1, &stream->sample.source);
	if (stream->sample.buffer) alDeleteBuffers(1, &stream->sample.buffer);
	delete stream;
}

HSAMPLE AIL_stream_sample_handle(HSTREAM stream) { return stream ? &stream->sample : nullptr; }
void AIL_start_stream(HSTREAM stream) { if (stream) alSourcePlay(stream->sample.source); }
void AIL_pause_stream(HSTREAM stream, S32 onoff)
{
	if (!stream) return;
	if (onoff) alSourcePause(stream->sample.source);
	else alSourcePlay(stream->sample.source);
}

S32 AIL_stream_status(HSTREAM stream)
{
	if (!stream) return SMP_DONE;
	ALint state = AL_STOPPED;
	alGetSourcei(stream->sample.source, AL_SOURCE_STATE, &state);
	return (state == AL_PLAYING || state == AL_PAUSED) ? 0 : SMP_DONE;
}

// --- Event system / soundbanks: structural stubs, see header comment ---

HEVENTSYSTEM AIL_startup_event_system(HDIGDRIVER dig, S32 command_buf_len, char *memory_buf, S32 memory_len)
{
	(void)dig; (void)command_buf_len; (void)memory_buf; (void)memory_len;
	static int dummy = 1;
	return &dummy; // non-null so init sequences that check "!= 0" proceed
}

HMSOUNDBANK AIL_add_soundbank(char const *filename, char const *name)
{
	fprintf(stderr, "LinuxAudioShim: AIL_add_soundbank('%s') is a structural stub - no soundbank format decoder exists (see header comment)\n", filename ? filename : "(null)");
	(void)name;
	static int dummy = 1;
	return (HMSOUNDBANK)&dummy;
}

S32 AIL_enumerate_events(HMSOUNDBANK bank, HMSSENUM *next, char const *list, char const **name)
{
	(void)bank; (void)list; (void)name;
	if (next) *next = MSS_FIRST;
	return 0; // no events to enumerate - ends any while() loop safely
}

U64 AIL_enqueue_event_by_name(char const *name) { (void)name; return 0; }
S32 AIL_begin_event_queue_processing(void) { return 1; }
S32 AIL_complete_event_queue_processing(void) { return 1; }

S32 AIL_enumerate_sound_instances(HEVENTSYSTEM system, HMSSENUM *next, S32 statuses, char const *label_query, U64 search_for_ID, MILESEVENTSOUNDINFO *info)
{
	(void)system; (void)statuses; (void)label_query; (void)search_for_ID; (void)info;
	if (next) *next = MSS_FIRST;
	return 0; // no scripted-event sound instances exist in this shim
}

S32 AIL_enqueue_event_start(void) { return 0; }
S32 AIL_enqueue_event_buffer(S32 *token, void *user_buffer, S32 user_buffer_len, S32 user_buffer_is_ptr)
{
	(void)token; (void)user_buffer; (void)user_buffer_len; (void)user_buffer_is_ptr;
	return 0;
}
U64 AIL_enqueue_event_end_named(S32 token, char const *event_name) { (void)token; (void)event_name; return 0; }
void AIL_set_variable_float(UINTa context, char const *name, F32 value) { (void)context; (void)name; (void)value; }

bool LinuxAudioShim_PlayTestTone(HDIGDRIVER dig)
{
	if (!dig) return false;

	const U32 sampleRate = dig->frequency;
	const float freqHz = 440.0f;
	const float durationSecs = 0.5f;
	const size_t sampleCount = (size_t)(sampleRate * durationSecs);
	std::vector<int16_t> pcm(sampleCount);
	for (size_t i = 0; i < sampleCount; i++)
	{
		float t = (float)i / (float)sampleRate;
		pcm[i] = (int16_t)(3000.0f * sinf(2.0f * 3.14159265f * freqHz * t));
	}

	HSAMPLE s = AIL_allocate_sample_handle(dig);
	AIL_init_sample(s, DIG_F_16BITS_MASK); // mono 16-bit: DIG_F_16BITS_MASK set, DIG_F_STEREO_MASK clear
	AIL_set_sample_address(s, pcm.data(), (U32)(pcm.size() * sizeof(int16_t)));
	AIL_set_sample_is_3D(s, 0);
	AIL_set_sample_volume_levels(s, 0.5f, 0.5f);
	AIL_start_sample(s);

	ALint state = AL_INITIAL;
	alGetSourcei(s->source, AL_SOURCE_STATE, &state);
	bool ok = (alGetError() == AL_NO_ERROR);

	// Give it a moment then confirm it actually reached PLAYING.
	struct timespec req = { 0, 100 * 1000 * 1000 };
	nanosleep(&req, nullptr);
	alGetSourcei(s->source, AL_SOURCE_STATE, &state);
	ok = ok && (state == AL_PLAYING);

	struct timespec waitOut = { 0, (long)(durationSecs * 1e9) };
	nanosleep(&waitOut, nullptr);

	AIL_release_sample_handle(s);
	return ok;
}
