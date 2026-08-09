/*
 * LinuxSceShim.c - the Sony OS calls the vendor PS4 Miles libraries make.
 *
 * patch_orbis_miles.py keeps every object in mssorbis.a, including Sony's own
 * threading, timing, file and audio-driver code. What those objects reference is a
 * small, closed set of 29 sce* entry points, and this file provides them over
 * pthreads, POSIX and SDL2. Keeping the vendor objects is what lets us skip
 * reimplementing rrThread/rrSemaphore and the RADSS driver vtable - two more
 * caller-allocated layouts that would have to be recovered by disassembly.
 *
 * EVERY signature below was read off `objdump -dr` at the call site in the object
 * that references it, never inferred from Sony's documented API. The two differ in
 * places that matter, and the port's history is unambiguous that guessing here
 * corrupts memory silently rather than failing (see the method note at the end of
 * .claude/linux-port/IGGY.md). The comment on each group records what was seen.
 *
 * Nothing here is reachable from the rest of the client: these names appear only as
 * undefined symbols in the Miles archive.
 */

/* CPU_SET/pthread_setaffinity_np/pthread_setname_np are all glibc extensions. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>

/* ------------------------------------------------------------------ *
 *  Error codes                                                       *
 * ------------------------------------------------------------------ *
 *
 * Sony returns 0 on success and 0x80020000|errno on failure, and the vendor code
 * compares against literal values: rrMutexLockTimeout tests 0x8002003c on the
 * timed-lock path and 0x80020010 on the try-lock path. Those are FreeBSD's
 * ETIMEDOUT (60) and EBUSY (16).
 *
 * EBUSY happens to match on Linux; ETIMEDOUT does not (110 here, 60 there). So the
 * errno must be translated to FreeBSD's numbering, not passed through - otherwise a
 * lock timeout is reported as an unknown error and rrMutexLockTimeout returns
 * "acquired" for a mutex it never got.
 */
#define SCE_ERRNO_BASE 0x80020000u

static int sce_error(int err)
{
   int bsd;

   switch (err) {
      case ETIMEDOUT: bsd = 60; break;   /* Linux 110 */
      case EAGAIN:    bsd = 35; break;   /* Linux 11  */
      case EDEADLK:   bsd = 11; break;   /* Linux 35  */
      default:        bsd = err; break;  /* EBUSY/EINVAL/ENOMEM/... agree */
   }
   return (int)(SCE_ERRNO_BASE | (unsigned)bsd);
}

/* ------------------------------------------------------------------ *
 *  scePthread - threads                                              *
 * ------------------------------------------------------------------ *
 *
 * ORBIS_rrThreads.obj's rrThreadCreate shows the whole shape:
 *
 *    lea  0x60(%rbx),%r15          ; &thread->handle  -> ScePthread *
 *    lea  -0x68(%rbp),%r14         ; &attr            -> ScePthreadAttr *
 *    lea  orbis_shim(%rip),%rdx    ; entry
 *    mov  %r15,%rdi / %r14,%rsi / -0x98(%rbp),%rcx / r8 = name or NULL
 *    call scePthreadCreate
 *
 * so Create takes the handle *by address* and a trailing name, while Join, Detach,
 * Setaffinity and Set/Getschedparam all load 0x60(%rbx) and pass the handle *by
 * value*. ScePthread and ScePthreadAttr are both pointer-sized opaque handles, so
 * pthread_t drops straight in and the attr slot holds a pthread_attr_t we own.
 */
typedef pthread_t ScePthread;

static int sce_attr_alloc(pthread_attr_t **slot)
{
   pthread_attr_t *attr = (pthread_attr_t *)calloc(1, sizeof(*attr));
   int rc;

   if (attr == NULL)
      return sce_error(ENOMEM);
   rc = pthread_attr_init(attr);
   if (rc != 0) {
      free(attr);
      return sce_error(rc);
    }
   *slot = attr;
   return 0;
}

int scePthreadAttrInit(pthread_attr_t **attr)
{
   return sce_attr_alloc(attr);
}

int scePthreadAttrDestroy(pthread_attr_t **attr)
{
   if (attr == NULL || *attr == NULL)
      return sce_error(EINVAL);
   pthread_attr_destroy(*attr);
   free(*attr);
   *attr = NULL;
   return 0;
}

/* esi holds the size, so it is a 32-bit count of bytes. */
int scePthreadAttrSetstacksize(pthread_attr_t **attr, unsigned int stack_size)
{
   if (attr == NULL || *attr == NULL)
      return sce_error(EINVAL);
   /* Below PTHREAD_STACK_MIN glibc refuses outright where Sony would clamp. */
   if (stack_size < (unsigned int)PTHREAD_STACK_MIN)
      stack_size = (unsigned int)PTHREAD_STACK_MIN;
   return pthread_attr_setstacksize(*attr, stack_size) == 0 ? 0 : sce_error(errno);
}

int scePthreadCreate(ScePthread *thread, pthread_attr_t **attr,
                     void *(*entry)(void *), void *arg, const char *name)
{
   int rc = pthread_create(thread, (attr != NULL) ? *attr : NULL, entry, arg);

   if (rc != 0)
      return sce_error(rc);
#ifdef __GLIBC__
   /* Sony names the thread here; the equivalent is useful in gdb/perf and free. */
   if (name != NULL && *name != '\0') {
      char trimmed[16];                    /* pthread_setname_np limit, incl. NUL */
      snprintf(trimmed, sizeof(trimmed), "%s", name);
      pthread_setname_np(*thread, trimmed);
   }
#else
   (void)name;
#endif
   return 0;
}

int scePthreadJoin(ScePthread thread, void **value)
{
   int rc = pthread_join(thread, value);
   return rc == 0 ? 0 : sce_error(rc);
}

int scePthreadDetach(ScePthread thread)
{
   int rc = pthread_detach(thread);
   return rc == 0 ? 0 : sce_error(rc);
}

ScePthread scePthreadSelf(void)
{
   return pthread_self();
}

int scePthreadYield(void)
{
   sched_yield();
   return 0;
}

/*
 * Affinity. rrThreadSetCPUCore builds `1 << core` in esi and passes the handle in
 * rdi, i.e. a plain CPU bitmask. Failure is deliberately not propagated: this is a
 * scheduling hint, the caller treats a non-zero return as an error, and a container
 * or restricted cpuset must not be able to fail thread setup over it.
 */
int scePthreadSetaffinity(ScePthread thread, unsigned long long mask)
{
   cpu_set_t set;
   unsigned i;

   CPU_ZERO(&set);
   for (i = 0; i < 64 && i < CPU_SETSIZE; ++i)
      if (mask & (1ull << i))
         CPU_SET(i, &set);

   if (CPU_COUNT(&set) > 0)
      pthread_setaffinity_np(thread, sizeof(set), &set);
   return 0;
}

/*
 * Scheduling. Get is called on the calling thread right after scePthreadSelf, and
 * Set feeds the result back with a different priority. SceKernelSchedParam is a
 * single int, which is struct sched_param's layout.
 *
 * Both report success unconditionally. Under SCHED_OTHER - what every thread here
 * gets without CAP_SYS_NICE - priority is fixed at 0 and pthread_setschedparam
 * returns EINVAL/EPERM for anything else. Sony's priorities are meaningless on
 * Linux anyway, and rrThreadCreate aborts thread creation if this fails.
 */
int scePthreadGetschedparam(ScePthread thread, int *policy, struct sched_param *param)
{
   if (pthread_getschedparam(thread, policy, param) != 0) {
      if (policy != NULL)
         *policy = SCHED_OTHER;
      if (param != NULL)
         param->sched_priority = 0;
   }
   return 0;
}

int scePthreadSetschedparam(ScePthread thread, int policy, const struct sched_param *param)
{
   if (param != NULL)
      pthread_setschedparam(thread, policy, param);
   return 0;
}

/* ------------------------------------------------------------------ *
 *  scePthread - TLS keys                                             *
 * ------------------------------------------------------------------ *
 *
 * `lea -0x2c(%rbp),%rdi` for Create and `mov thread_tls_index(%rip),%edi` for
 * Get/Set: the key is a 32-bit value, held in a 4-byte slot. pthread_key_t is
 * exactly that. Get returns the pointer in rax with no error channel.
 */
int scePthreadKeyCreate(pthread_key_t *key, void (*destructor)(void *))
{
   int rc = pthread_key_create(key, destructor);
   return rc == 0 ? 0 : sce_error(rc);
}

void *scePthreadGetspecific(pthread_key_t key)
{
   return pthread_getspecific(key);
}

int scePthreadSetspecific(pthread_key_t key, const void *value)
{
   int rc = pthread_setspecific(key, value);
   return rc == 0 ? 0 : sce_error(rc);
}

/* ------------------------------------------------------------------ *
 *  scePthread - mutexes                                              *
 * ------------------------------------------------------------------ *
 *
 * rrMutexCreate is the reference, and it matches what LinuxRadShim.c already
 * documents for the Iggy side: the mutex is caller-allocated. It aligns a pointer
 * inside the caller's buffer, stores it at self+0x78, and hands *that* address to
 * scePthreadMutexInit; the flags word lives at storage+8. So the OS handle occupies
 * storage[0..7] and every scePthreadMutex* call takes `ScePthreadMutex *`, i.e. the
 * address of that 8-byte slot - never the handle by value. Getting this backwards
 * would write a pthread_mutex_t over the flags word.
 *
 * scePthreadMutexattrSettype is always called with 2, Sony's RECURSIVE, so the
 * mutexes really are recursive and a plain PTHREAD_MUTEX_DEFAULT would self-deadlock.
 */
int scePthreadMutexattrInit(pthread_mutexattr_t **attr)
{
   pthread_mutexattr_t *a = (pthread_mutexattr_t *)calloc(1, sizeof(*a));

   if (a == NULL)
      return sce_error(ENOMEM);
   pthread_mutexattr_init(a);
   *attr = a;
   return 0;
}

int scePthreadMutexattrSettype(pthread_mutexattr_t **attr, int type)
{
   if (attr == NULL || *attr == NULL)
      return sce_error(EINVAL);
   /* 2 == SCE_PTHREAD_MUTEX_RECURSIVE, the only value the vendor code passes. */
   return pthread_mutexattr_settype(*attr,
                                    (type == 2) ? PTHREAD_MUTEX_RECURSIVE
                                                : PTHREAD_MUTEX_NORMAL) == 0
             ? 0 : sce_error(errno);
}

int scePthreadMutexInit(pthread_mutex_t **mutex, pthread_mutexattr_t **attr,
                        const char *name)
{
   pthread_mutex_t *m;
   int rc;

   (void)name;
   if (mutex == NULL)
      return sce_error(EINVAL);

   m = (pthread_mutex_t *)calloc(1, sizeof(*m));
   if (m == NULL)
      return sce_error(ENOMEM);

   rc = pthread_mutex_init(m, (attr != NULL) ? *attr : NULL);
   if (rc != 0) {
      free(m);
      return sce_error(rc);
   }
   *mutex = m;
   return 0;
}

int scePthreadMutexDestroy(pthread_mutex_t **mutex)
{
   if (mutex == NULL || *mutex == NULL)
      return sce_error(EINVAL);
   pthread_mutex_destroy(*mutex);
   free(*mutex);
   *mutex = NULL;
   return 0;
}

int scePthreadMutexLock(pthread_mutex_t **mutex)
{
   int rc;

   if (mutex == NULL || *mutex == NULL)
      return sce_error(EINVAL);
   rc = pthread_mutex_lock(*mutex);
   return rc == 0 ? 0 : sce_error(rc);
}

int scePthreadMutexUnlock(pthread_mutex_t **mutex)
{
   int rc;

   if (mutex == NULL || *mutex == NULL)
      return sce_error(EINVAL);
   rc = pthread_mutex_unlock(*mutex);
   return rc == 0 ? 0 : sce_error(rc);
}

int scePthreadMutexTrylock(pthread_mutex_t **mutex)
{
   int rc;

   if (mutex == NULL || *mutex == NULL)
      return sce_error(EINVAL);
   rc = pthread_mutex_trylock(*mutex);
   return rc == 0 ? 0 : sce_error(rc);
}

/*
 * The timeout is in microseconds: rrMutexLockTimeout takes milliseconds and does
 * `imul $0x3e8,%esi,%esi` immediately before the call. pthread_mutex_timedlock
 * wants an absolute CLOCK_REALTIME deadline instead.
 */
int scePthreadMutexTimedlock(pthread_mutex_t **mutex, unsigned int usec)
{
   struct timespec deadline;
   int rc;

   if (mutex == NULL || *mutex == NULL)
      return sce_error(EINVAL);

   clock_gettime(CLOCK_REALTIME, &deadline);
   deadline.tv_sec  += (time_t)(usec / 1000000u);
   deadline.tv_nsec += (long)(usec % 1000000u) * 1000L;
   if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_nsec -= 1000000000L;
      deadline.tv_sec  += 1;
   }

   rc = pthread_mutex_timedlock(*mutex, &deadline);
   return rc == 0 ? 0 : sce_error(rc);
}

/* ------------------------------------------------------------------ *
 *  POSIX semaphores, interposed                                      *
 * ------------------------------------------------------------------ *
 *
 * patch_orbis_miles.py rewrites ORBIS_rrThreads.obj's references to sem_* into these,
 * so the override applies to Miles alone rather than to the whole client. Two
 * independent reasons, both read out of rrSemaphoreCreate and
 * rrSemaphoreDecrementOrWait:
 *
 * 1. THE SEMAPHORE IS CALLER-ALLOCATED, AND SIZED FOR FREEBSD.
 *    rrSemaphoreCreate aligns a pointer inside the caller's buffer, stores it at
 *    self+0x78, writes the initial count to storage+0x14, calls sem_init(storage,...),
 *    then stamps the magic 0x231d at storage+0x10. So it believes a sem_t is at most
 *    16 bytes - true on FreeBSD, where sem_t is a small struct. glibc's is 32, so
 *    sem_init would run over both the magic and the count, and RAD's magic write would
 *    then land inside glibc's semaphore state. This is the same caller-allocated
 *    hazard LinuxRadShim.c documents for rrMutexCreate.
 *
 *    Fixed by keeping only a pointer in the caller's buffer and putting the real
 *    sem_t on the heap: eight bytes used, storage+0x10 and +0x14 left alone.
 *
 * 2. ERRNO IS NUMBERED DIFFERENTLY.
 *    rrSemaphoreDecrementOrWait tests errno against 0x3c after a failed
 *    sem_timedwait, then against 4, and executes `ud2` if it is neither. Those are
 *    FreeBSD's ETIMEDOUT (60) and EINTR (4). EINTR agrees; ETIMEDOUT is 110 on Linux,
 *    so a timed wait that legitimately timed out killed the process with SIGILL. The
 *    timeout is a normal event on Miles' timer thread, so this fired within a second
 *    of the first sound playing.
 *
 *    errno is set to FreeBSD's value here rather than translated at the reader,
 *    because the reader is vendor code. It is thread-local and consumed immediately.
 */
static int sce_sem_fail(int freebsd_errno)
{
   errno = freebsd_errno;
   return -1;
}

int mss_sem_init(void **slot, int pshared, unsigned int value)
{
   sem_t *sem;

   if (slot == NULL)
      return sce_sem_fail(EINVAL);

   sem = (sem_t *)calloc(1, sizeof(*sem));
   if (sem == NULL)
      return sce_sem_fail(ENOMEM);

   if (sem_init(sem, pshared, value) != 0) {
      int err = errno;
      free(sem);
      return sce_sem_fail(err);
   }
   *slot = sem;
   return 0;
}

int mss_sem_destroy(void **slot)
{
   if (slot == NULL || *slot == NULL)
      return sce_sem_fail(EINVAL);
   sem_destroy((sem_t *)*slot);
   free(*slot);
   *slot = NULL;
   return 0;
}

int mss_sem_wait(void **slot)
{
   if (slot == NULL || *slot == NULL)
      return sce_sem_fail(EINVAL);
   /* EINTR (4) agrees between the two libcs, and the caller retries on it. */
   return sem_wait((sem_t *)*slot);
}

int mss_sem_post(void **slot)
{
   if (slot == NULL || *slot == NULL)
      return sce_sem_fail(EINVAL);
   return sem_post((sem_t *)*slot);
}

int mss_sem_timedwait(void **slot, const struct timespec *abstime)
{
   if (slot == NULL || *slot == NULL)
      return sce_sem_fail(EINVAL);

   if (sem_timedwait((sem_t *)*slot, abstime) == 0)
      return 0;

   /* The one translation that matters - see (2) above. */
   if (errno == ETIMEDOUT)
      return sce_sem_fail(60);
   return -1;
}

/* ------------------------------------------------------------------ *
 *  sceKernel - time and sleeping                                     *
 * ------------------------------------------------------------------ *
 *
 * The unit is microseconds: the call sites pass 0x3e8 (1 ms) in rrThreadCreate's
 * spin and 0x1f4 (500 us) in the RADSS mixer thread's underrun wait.
 */
int sceKernelUsleep(unsigned int usec)
{
   usleep(usec);
   return 0;
}

/*
 * SceKernelTimeval is {int64 tv_sec; int64 tv_usec}, which is glibc's struct timeval
 * on x86-64. Declared explicitly rather than casting so the layout is stated, not
 * assumed.
 */
typedef struct {
   int64_t tv_sec;
   int64_t tv_usec;
} SceKernelTimeval;

int sceKernelGettimeofday(SceKernelTimeval *tv)
{
   struct timeval now;

   if (tv == NULL)
      return sce_error(EINVAL);
   gettimeofday(&now, NULL);
   tv->tv_sec  = (int64_t)now.tv_sec;
   tv->tv_usec = (int64_t)now.tv_usec;
   return 0;
}

/* ------------------------------------------------------------------ *
 *  sceKernel - files                                                 *
 * ------------------------------------------------------------------ *
 *
 * orbisio.obj is four one-line functions and they are all pass-through:
 * Platform_OpenFile calls sceKernelOpen(path, 0, 0), Platform_ReadFile forwards
 * (fd, buf, len), Platform_SeekFromBeginning passes whence 0, Platform_CloseFile
 * passes the fd. The descriptor is held as a 32-bit int at offset 0 of Miles' file
 * struct, so these really are POSIX descriptors. Only O_RDONLY (0) is ever used.
 */
int sceKernelOpen(const char *path, int flags, int mode)
{
   int fd = open(path, flags, (mode_t)mode);
   return fd >= 0 ? fd : sce_error(errno);
}

int64_t sceKernelRead(int fd, void *buf, size_t count)
{
   ssize_t got = read(fd, buf, count);
   return got >= 0 ? (int64_t)got : (int64_t)sce_error(errno);
}

int64_t sceKernelLseek(int fd, int64_t offset, int whence)
{
   off_t pos = lseek(fd, (off_t)offset, whence);
   return pos >= 0 ? (int64_t)pos : (int64_t)sce_error(errno);
}

int sceKernelClose(int fd)
{
   return close(fd) == 0 ? 0 : sce_error(errno);
}

/* ------------------------------------------------------------------ *
 *  sceAudioOut - the output device                                   *
 * ------------------------------------------------------------------ *
 *
 * This is the only group that is more than a rename, and RADSS_OrbisOpenDriver
 * pins down every parameter:
 *
 *    mov $0xff,%edi          ; user id  (system user)
 *    mov $0x0,%esi           ; port type MAIN
 *    xor %edx,%edx           ; index 0
 *    mov $0x300,%ecx         ; 768 sample frames per grain
 *    mov $0xbb80,%r8d        ; 48000 Hz
 *    r9d = (channels == 2) ? 1 : 2
 *    call sceAudioOutOpen
 *
 * so the format is signed 16-bit, 48 kHz, and param 1/2 selects stereo or 8-channel.
 * SetVolume is then called with 0xff (all channels) and eight 0x8000 entries, which
 * is Sony's 0 dB.
 *
 * RADSS_Thread submits exactly one grain per sceAudioOutOutput and relies on the
 * call *blocking* until the device has room - that back-pressure is the mixer's
 * entire clock. SDL2's queued audio is the direct equivalent, so long as we wait
 * here rather than letting the queue grow without bound.
 */
#define SCE_AUDIO_OUT_MAX_PORTS 4
#define SCE_AUDIO_VOLUME_0DB    0x8000

typedef struct {
   SDL_AudioDeviceID device;
   unsigned          channels;
   unsigned          grain_frames;
   unsigned          bytes_per_grain;
   int               in_use;
   /* MCLINUX_MILES_AUDIO_DEBUG only - see sce_audio_report(). */
   unsigned long     grains;
   int               peak;
} SceAudioPort;

/*
 * Silence is indistinguishable from "not wired up" by ear, and the port's history is
 * that reasoning about which of the two you have produces a confident wrong answer.
 * MCLINUX_MILES_AUDIO_DEBUG=1 prints the SDL backend actually in use and, once a
 * second, how many grains Miles has submitted and their peak amplitude - so "the
 * mixer is running but the samples are zero" and "no samples are arriving at all"
 * can be told apart without hearing anything.
 */
static int sce_audio_debug(void)
{
   static int state = -1;

   if (state < 0) {
      const char *env = getenv("MCLINUX_MILES_AUDIO_DEBUG");
      state = (env != NULL && *env != '\0' && *env != '0') ? 1 : 0;
   }
   return state;
}

static void sce_audio_report(SceAudioPort *port)
{
   unsigned long per_second;

   if (!sce_audio_debug() || port->grain_frames == 0)
      return;

   per_second = 48000ul / port->grain_frames;
   if (per_second == 0 || (port->grains % per_second) != 0)
      return;

   fprintf(stderr, "[miles] %lu grains submitted, peak %d/32767, %u bytes queued\n",
           port->grains, port->peak, SDL_GetQueuedAudioSize(port->device));
   port->peak = 0;
}

static SceAudioPort  sce_audio_ports[SCE_AUDIO_OUT_MAX_PORTS];
static pthread_mutex_t sce_audio_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned sce_audio_channels_for_param(unsigned param)
{
   switch (param) {
      case 0:  return 1;   /* S16 mono   */
      case 1:  return 2;   /* S16 stereo */
      case 2:  return 8;   /* S16 8ch    */
      default: return 2;
   }
}

int sceAudioOutOpen(int user_id, int port_type, int index,
                    unsigned int grain, unsigned int freq, unsigned int param)
{
   SDL_AudioSpec want, have;
   SceAudioPort *port = NULL;
   int handle = -1;
   int i;

   (void)user_id;
   (void)port_type;
   (void)index;

   if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
      fprintf(stderr, "[miles] SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
      return sce_error(ENODEV);
   }

   pthread_mutex_lock(&sce_audio_lock);
   for (i = 0; i < SCE_AUDIO_OUT_MAX_PORTS; ++i) {
      if (!sce_audio_ports[i].in_use) {
         port = &sce_audio_ports[i];
         handle = i + 1;              /* 0 is a valid Sony handle; keep ours > 0 */
         port->in_use = 1;
         break;
      }
   }
   pthread_mutex_unlock(&sce_audio_lock);

   if (port == NULL)
      return sce_error(EMFILE);

   SDL_zero(want);
   want.freq     = (int)freq;
   want.format   = AUDIO_S16SYS;
   want.channels = (Uint8)sce_audio_channels_for_param(param);
   want.samples  = (Uint16)grain;
   want.callback = NULL;                /* queued audio, not a pull callback */

   /* No conversion allowed: Miles writes exactly this format and nothing else. */
   port->device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
   if (port->device == 0) {
      fprintf(stderr, "[miles] SDL_OpenAudioDevice(%u Hz, %u ch) failed: %s\n",
              freq, want.channels, SDL_GetError());
      port->in_use = 0;
      return sce_error(ENODEV);
   }

   port->channels        = have.channels;
   port->grain_frames    = grain;
   port->bytes_per_grain = grain * have.channels * sizeof(int16_t);

   if (sce_audio_debug())
      fprintf(stderr, "[miles] SDL audio backend \"%s\": %d Hz, %u ch, %u frames/grain\n",
              SDL_GetCurrentAudioDriver(), have.freq, have.channels, grain);

   SDL_PauseAudioDevice(port->device, 0);
   return handle;
}

static SceAudioPort *sce_audio_port(int handle)
{
   if (handle < 1 || handle > SCE_AUDIO_OUT_MAX_PORTS)
      return NULL;
   return sce_audio_ports[handle - 1].in_use ? &sce_audio_ports[handle - 1] : NULL;
}

/*
 * Blocking submit. The wait is what paces the mixer thread; without it Miles would
 * spin and the queue would grow until the latency was seconds. Two grains of slack
 * is enough to absorb scheduling jitter (32 ms at 768 frames / 48 kHz) while keeping
 * sound effects prompt.
 */
int sceAudioOutOutput(int handle, const void *samples)
{
   SceAudioPort *port = sce_audio_port(handle);
   Uint32 high_water;

   if (port == NULL)
      return sce_error(EINVAL);
   if (samples == NULL)                  /* Sony's "drain" call */
      return 0;

   high_water = (Uint32)port->bytes_per_grain * 2u;
   while (SDL_GetQueuedAudioSize(port->device) > high_water)
      SDL_Delay(1);

   if (SDL_QueueAudio(port->device, samples, port->bytes_per_grain) != 0)
      return sce_error(EIO);

   port->grains++;
   if (sce_audio_debug()) {
      const int16_t *pcm = (const int16_t *)samples;
      unsigned n = port->grain_frames * port->channels;
      unsigned i;

      for (i = 0; i < n; ++i) {
         int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
         if (v > port->peak)
            port->peak = v;
      }
      sce_audio_report(port);
   }
   return (int)port->grain_frames;
}

int sceAudioOutSetVolume(int handle, int flags, int *volumes)
{
   SceAudioPort *port = sce_audio_port(handle);

   (void)flags;
   (void)volumes;
   /*
    * Nothing to do: the vendor driver only ever sets 0 dB on every channel, and all
    * real level control happens in Miles' own mixer before the samples reach us.
    */
   return port != NULL ? 0 : sce_error(EINVAL);
}

int sceAudioOutClose(int handle)
{
   SceAudioPort *port = sce_audio_port(handle);

   if (port == NULL)
      return sce_error(EINVAL);

   SDL_CloseAudioDevice(port->device);
   port->device = 0;
   port->in_use = 0;
   return 0;
}

/* ------------------------------------------------------------------ *
 *  FreeBSD libc internals                                            *
 * ------------------------------------------------------------------ */

/* FreeBSD spells errno's address __error(); glibc spells it __errno_location(). */
int *__error(void)
{
   return __errno_location();
}

/*
 * _FLog / _FSin are the float twins of the Dinkumware helpers LinuxRadShim.c
 * already provides for Iggy as _Log / _Sin.
 *
 * _FLog(x, 1) is log10, established from the one call site in mileseventexec.obj:
 * the result is multiplied by the .rodata constant 20.0f, and the branch taken when
 * x < 1e-5f returns -96.0f instead. That is a linear-amplitude-to-decibel
 * conversion with a floor, which only works out if the log is base 10.
 *
 * Note this contradicts the base-2 guess in LinuxRadShim.c's _Log, which was never
 * reached from Iggy and so was never tested. The evidence here is the first real
 * measurement of the non-zero flag; both files now agree on log10.
 *
 * _FSin's only call site passes edi = 0, matching _Sin's "0 -> sin, 1 -> cos".
 */
float _FLog(float x, int base_flag)
{
   return base_flag ? log10f(x) : logf(x);
}

float _FSin(float x, unsigned int quadrant_offset, unsigned int unused)
{
   (void)unused;
   return (quadrant_offset & 1u) ? cosf(x) : sinf(x);
}

/*
 * There is deliberately no RADSS driver here. The vendor's own RADSS_Orbis one is
 * kept (radss_orbis.obj is not dropped) and runs on the sceAudioOut* implementation
 * above; LinuxMiles.h points AIL_open_digital_driver at its renamed symbol.
 */
