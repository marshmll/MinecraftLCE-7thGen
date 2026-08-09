/*
 * miles_spike - prove the vendor Miles runtime works on Linux without the game.
 *
 * The Iggy port's lesson was that a 500 kLOC client is the worst place to find out a
 * relinked blob is unhappy, so this is the audio equivalent of iggy_spike: it runs
 * the exact sequence SoundEngine::init() runs (Minecraft.Client/Common/Audio/
 * SoundEngine.cpp:214-359) and stops at the first thing that fails, with the vendor's
 * own AIL_last_error() text.
 *
 *   miles_spike <bank.msscmp>                  list every event in the bank
 *   miles_spike <bank.msscmp> <event> [secs]   play one event and pump the queue
 *
 * Run it from Minecraft.Client/, e.g.
 *   ../build/Minecraft.Client/Linux/Miles/miles_spike Durango/Sound/Minecraft.msscmp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "LinuxMiles.h"

/*
 * Before anything else, check that LinuxMiles.h really did select the PS4 layout.
 * If mss.h is ever included some other way, or the header's platform trick stops
 * working, every struct shared with the blob silently changes shape - and MAX_SPEAKERS
 * is the cheapest witness, being 8 only on the PS4 branch and 6 under IS_LINUX.
 * MSS_STATIC_RIB is the second: without it Register_RIB(BinkADec) does not exist and
 * the music codec would never be registered.
 */
#if MAX_SPEAKERS != 8
#error "mss.h was not compiled with the PS4 ABI - include LinuxMiles.h, not mss.h."
#endif
#if !defined(MSS_STATIC_RIB)
#error "mss.h was not compiled for static RIB providers - see LinuxMiles.h."
#endif

static void fail(const char *what)
{
   fprintf(stderr, "miles_spike: %s failed: %s\n", what, AIL_last_error());
   exit(1);
}

int main(int argc, char **argv)
{
   const char *bank_path;
   const char *event_name = NULL;
   double seconds = 3.0;
   HDIGDRIVER dig;
   HMSOUNDBANK bank;
   HMSSENUM token;
   char const *name = NULL;
   int event_count = 0;
   int ticks;

   if (argc < 2) {
      fprintf(stderr,
              "usage: %s <bank.msscmp> [event-name] [seconds]\n"
              "  with no event name, lists the bank's events and exits\n",
              argv[0]);
      return 2;
   }
   bank_path = argv[1];
   if (argc > 2)
      event_name = argv[2];
   if (argc > 3)
      seconds = atof(argv[3]);

   /* Bink Audio. The .binka music files and the compressed sounds in the bank both
    * decode through this provider, and nothing registers it implicitly. */
   Register_RIB(BinkADec);

   if (AIL_startup() == 0)
      fail("AIL_startup");

   /* 48 kHz / 16-bit / stereo, matching what Orbis asks for in SoundEngine::init.
    * The driver behind this is the vendor's own RADSS_Orbis one, running on the
    * sceAudioOut* implementation in LinuxSceShim.c. */
   dig = AIL_open_digital_driver(48000, 16, 2, 0);
   if (dig == 0)
      fail("AIL_open_digital_driver");
   printf("miles_spike: digital driver open\n");

   /* Same sizes SoundEngine::init uses; the defaults are too small for this bank. */
   if (AIL_startup_event_system(dig, 1024 * 20, 0, 1024 * 128) == 0)
      fail("AIL_startup_event_system");
   printf("miles_spike: event system up\n");

   bank = AIL_add_soundbank(bank_path, 0);
   if (bank == NULL) {
      fprintf(stderr, "miles_spike: AIL_add_soundbank(\"%s\") failed: %s\n",
              bank_path, AIL_last_error());
      fprintf(stderr, "  (paths are relative to the working directory - run this "
                      "from Minecraft.Client/)\n");
      return 1;
   }
   printf("miles_spike: loaded %s\n", bank_path);

   token = MSS_FIRST;
   while (AIL_enumerate_events(bank, &token, 0, &name)) {
      if (event_name == NULL)
         printf("  %4d  %s\n", event_count, name);
      event_count++;
   }
   printf("miles_spike: %d events in the bank\n", event_count);
   if (event_count == 0) {
      fprintf(stderr, "miles_spike: bank loaded but enumerated nothing - the loader "
                      "accepted it without decoding it.\n");
      return 1;
   }

   if (event_name == NULL)
      return 0;

   /* SoundEngine::init does this immediately after loading the bank; the sounds a
    * "cache" event references are not resident until it has run. */
   AIL_enqueue_event_by_name("Minecraft/CacheSounds");

   /* A listener at the origin facing -z, so a 3D event is audible rather than
    * attenuated to nothing before it starts. */
   AIL_set_listener_3D_position(dig, 0.0f, 0.0f, 0.0f);
   AIL_set_listener_3D_orientation(dig, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f);

   printf("miles_spike: enqueue \"%s\" -> id %llu\n", event_name,
          (unsigned long long)AIL_enqueue_event_by_name(event_name));

   /* The event queue only advances between begin/complete, which is what
    * SoundEngine::tick does once a frame. 60 Hz here for the same reason. */
   for (ticks = 0; ticks < (int)(seconds * 60.0); ++ticks) {
      MILESEVENTSOUNDINFO info;
      HMSSENUM sound = MSS_FIRST;
      int pending = 0, playing = 0, complete = 0;

      AIL_begin_event_queue_processing();
      while (AIL_enumerate_sound_instances(0, &sound, 0, 0, 0, &info)) {
         if (info.Status == MILESEVENT_SOUND_STATUS_PENDING)  pending++;
         if (info.Status == MILESEVENT_SOUND_STATUS_PLAYING)  playing++;
         if (info.Status == MILESEVENT_SOUND_STATUS_COMPLETE) complete++;
      }
      AIL_complete_event_queue_processing();

      if (ticks % 15 == 0)
         printf("miles_spike: t=%4d ms  pending=%d playing=%d complete=%d\n",
                ticks * 1000 / 60, pending, playing, complete);

      usleep(1000000 / 60);
   }

   printf("miles_spike: done\n");
   AIL_close_digital_driver(dig);
   AIL_shutdown();
   return 0;
}
