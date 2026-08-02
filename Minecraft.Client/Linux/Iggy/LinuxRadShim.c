/*
 * LinuxRadShim.c - the platform layer Iggy's portable core needs, for Linux.
 *
 * Iggy ships as one portable core plus a per-platform bottom half. In this tree the
 * core exists only as prebuilt libraries, but `Minecraft.Client/Orbis/Iggy/lib/
 * libiggy_orbis.a` happens to be ELF x86-64 with the SysV ABI - the same ABI Linux
 * uses - so the core itself is reusable. What is *not* reusable is its bottom half,
 * which calls Sony's `scePthread*`/`sceKernel*` and Sony's libc internals.
 *
 * The Linux build therefore drops four objects from that archive and this file
 * replaces exactly what they defined:
 *
 *   ORBIS_rrThreads.obj      -> rrMutex* (real), rrThread* (link-only, see below)
 *   ORBIS_rrTime.obj         -> rrGetTime/rrGetTicks/rrTimeTo*
 *   iggy_platform_deps.obj   -> iggy_default_system_alloc/free, iggy_platform_init,
 *                               the gmtime helpers, IggyWaitOnFence,
 *                               and the `iggy_fence_wait_ticks` *variable*
 *   radss_orbis.obj          -> RADSS_SonyInstallDriver (audio; never installed)
 *
 * ORBIS_rrAtomics.obj is deliberately *kept* - it is plain x86-64 with no Sony
 * dependency, so the core's atomics are the vendor's own.
 *
 * Every signature and semantic below was recovered by disassembling the vendor
 * objects it replaces (`objdump -dr`), not guessed. Where that mattered is called
 * out in comments - `rrMutex`'s caller-allocated layout and `rrTime`'s unit are both
 * things a plausible-looking guess would get wrong silently.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

/* Defined by the vendor's ORBIS_rrAtomics.obj, which we keep. Declared here rather
   than pulled from a header because rrCore.h in this tree declares rr* *types* only,
   never these functions. */
extern void rrAtomicMemoryBarrierFull(void);

/* ------------------------------------------------------------------ *
 *  Time                                                              *
 * ------------------------------------------------------------------ *
 *
 * The unit of "rrTime" is MICROSECONDS. This is not a guess:
 * ORBIS_rrTime.obj's rrTimePerSecond() is literally `mov eax,0xf4240; ret`
 * (1000000), rrTimeToMicros/rrMicrosToTime are the identity function, and
 * rrMillisToTime is `imul rax,rdi,0x3e8`.
 *
 * rrGetTime and rrGetTicks are the same function in the vendor object: microseconds
 * since the first call, forced monotonic. The monotonic clock gives us that for free.
 */

static uint64_t rad_now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

static uint64_t rad_epoch_us;   /* value of the first call, subtracted off */

static uint64_t rad_since_epoch_us(void)
{
   uint64_t now = rad_now_us();
   if (rad_epoch_us == 0)
      rad_epoch_us = now;
   return now - rad_epoch_us;
}

uint64_t rrGetTime (void) { return rad_since_epoch_us(); }
uint64_t rrGetTicks(void) { return rad_since_epoch_us(); }

uint64_t rrTimePerSecond (void)        { return 1000000ULL; }
uint64_t rrTimeToMicros  (uint64_t t)  { return t; }
uint64_t rrMicrosToTime  (uint64_t t)  { return t; }
uint64_t rrMillisToTime  (uint64_t t)  { return t * 1000ULL; }
uint64_t rrTicksToCycles (uint64_t t)  { return t; }

/* The vendor rounds rather than truncates: `lea rax,[rdi+0x1f4]` adds 500 first. */
uint64_t rrTimeToMillis  (uint64_t t)  { return (t + 500ULL) / 1000ULL; }

double   rrTimeToSeconds (uint64_t t)  { return (double)t / 1000000.0; }
double   rrTicksToSeconds(uint64_t t)  { return (double)t / 1000000.0; }

/* ------------------------------------------------------------------ *
 *  Mutexes                                                           *
 * ------------------------------------------------------------------ *
 *
 * IMPORTANT: rrMutex is CALLER-ALLOCATED. Iggy embeds it inside its own structs and
 * passes a pointer in; rrMutexCreate does not allocate and does not return a handle.
 * Returning a malloc'd pointer here - the obvious-looking implementation - would
 * corrupt whatever Iggy struct the mutex is embedded in.
 *
 * The vendor layout, straight out of ORBIS_rrThreads.obj's disassembly:
 *
 *   storage = (self + 15) & ~15      // 16-byte-aligned, inside the caller's buffer
 *   *(void **)(self + 0x78) = storage
 *   storage[0x0] = the OS mutex handle   (8 bytes; ScePthreadMutex is a pointer)
 *   storage[0x8] = U32 flags, bit 0x20 = "initialised"
 *
 * rrMutexLock/Unlock null-check, load storage from +0x78, test bit 0x20, and only
 * then touch the OS mutex - so a zeroed rrMutex is safely inert. We reproduce that
 * exactly, storing a pthread_mutex_t* in the 8-byte handle slot (glibc's
 * pthread_mutex_t is 40 bytes and would overrun the flags word if stored inline).
 *
 * The vendor makes it recursive (scePthreadMutexattrSettype(..., 2)); Iggy relies on
 * that, so we do the same.
 */

#define RRMUTEX_STORAGE_OFS   0x78
#define RRMUTEX_FLAG_INIT     0x20

static inline unsigned char *rrmutex_storage(void *self)
{
   return (unsigned char *)(((uintptr_t)self + 15u) & ~(uintptr_t)15u);
}

int rrMutexCreate(void *self, unsigned int flags)
{
   unsigned char *stor;
   pthread_mutex_t *m;
   pthread_mutexattr_t attr;

   if (!self)
      return 0;

   stor = rrmutex_storage(self);
   *(void **)((unsigned char *)self + RRMUTEX_STORAGE_OFS) = stor;
   *(uint32_t *)(stor + 8) = 0;

   m = (pthread_mutex_t *)malloc(sizeof *m);
   if (!m)
      return 0;

   pthread_mutexattr_init(&attr);
   pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
   if (pthread_mutex_init(m, &attr) != 0) {
      pthread_mutexattr_destroy(&attr);
      free(m);
      return 0;
   }
   pthread_mutexattr_destroy(&attr);

   *(void **)(stor + 0) = m;
   rrAtomicMemoryBarrierFull();
   *(uint32_t *)(stor + 8) = flags | RRMUTEX_FLAG_INIT;
   return 1;
}

/* Returns NULL unless the mutex is present and flagged initialised, mirroring the
   vendor's null-check-then-test-bit-0x20 guard. */
static pthread_mutex_t *rrmutex_live(void *self)
{
   unsigned char *stor;

   if (!self)
      return NULL;
   stor = *(unsigned char **)((unsigned char *)self + RRMUTEX_STORAGE_OFS);
   if (!stor || !(*(uint32_t *)(stor + 8) & RRMUTEX_FLAG_INIT))
      return NULL;
   return *(pthread_mutex_t **)(stor + 0);
}

void rrMutexDestroy(void *self)
{
   unsigned char *stor;
   pthread_mutex_t *m;

   if (!self)
      return;
   stor = *(unsigned char **)((unsigned char *)self + RRMUTEX_STORAGE_OFS);
   if (!stor)
      return;

   m = rrmutex_live(self);
   *(uint32_t *)(stor + 8) = 0;      /* clear the initialised bit first, as the vendor does */
   rrAtomicMemoryBarrierFull();
   if (m) {
      pthread_mutex_destroy(m);
      free(m);
      *(void **)(stor + 0) = NULL;
   }
}

int rrMutexLock(void *self)
{
   pthread_mutex_t *m = rrmutex_live(self);
   return m ? pthread_mutex_lock(m) : 0;
}

int rrMutexUnlock(void *self)
{
   pthread_mutex_t *m = rrmutex_live(self);
   return m ? pthread_mutex_unlock(m) : 0;
}

/* Vendor contract: timeout_ms == -1 means block forever and return 1; 0 means try
   once; otherwise wait that many milliseconds. Returns 1 on acquire, 0 on timeout. */
int rrMutexLockTimeout(void *self, int32_t timeout_ms)
{
   pthread_mutex_t *m = rrmutex_live(self);
   struct timespec ts;

   if (!m)
      return 1;                      /* inert mutex: the vendor returns success */
   if (timeout_ms < 0)
      return pthread_mutex_lock(m) == 0 ? 1 : 0;
   if (timeout_ms == 0)
      return pthread_mutex_trylock(m) == 0 ? 1 : 0;

   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec  += timeout_ms / 1000;
   ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
   if (ts.tv_nsec >= 1000000000L) {
      ts.tv_nsec -= 1000000000L;
      ts.tv_sec  += 1;
   }
   return pthread_mutex_timedlock(m, &ts) == 0 ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 *  Threads - link-only stubs, deliberately                           *
 * ------------------------------------------------------------------ *
 *
 * Checked with `nm -u` over every object we keep: rrThreadCreate, rrThreadWaitDone,
 * rrThreadSleep, rrThreadSetPriority and rrThreadCleanUp are referenced by
 * iggy_sound.obj and by nothing else. We never install an Iggy audio driver (see
 * RADSS_SonyInstallDriver below), so these are unreachable and only need to satisfy
 * the linker.
 *
 * They complain instead of failing silently: if one is ever reached, that means the
 * audio path came alive and this file needs a real pthread-backed rrThread layer -
 * which is a much bigger job, since rrThread has a caller-allocated layout with a
 * TLS index and a per-thread slot bitmap.
 */

static void rad_unimplemented(const char *what)
{
   static int warned;
   if (!warned) {
      warned = 1;
      fprintf(stderr,
              "[LinuxRadShim] %s was called, but the Iggy thread layer is a stub.\n"
              "               Only iggy_sound.obj references it and Iggy audio is\n"
              "               never installed on Linux - so reaching this means the\n"
              "               audio path is live and rrThread* now needs a real\n"
              "               implementation. See LinuxRadShim.c.\n", what);
   }
}

void *rrThreadCreate(void *self, void *fn, uint32_t stack_size, void *user,
                     uint32_t flags, const char *name)
{
   (void)self; (void)fn; (void)stack_size; (void)user; (void)flags; (void)name;
   rad_unimplemented("rrThreadCreate");
   return NULL;                      /* report failure rather than pretend */
}

void rrThreadWaitDone(void *self, void *result)
{
   (void)self; (void)result;
   rad_unimplemented("rrThreadWaitDone");
}

void rrThreadSetPriority(void *self, int priority)
{
   (void)self; (void)priority;
   rad_unimplemented("rrThreadSetPriority");
}

void rrThreadCleanUp(void)
{
   /* Called on shutdown paths; silently doing nothing is correct when no rrThread
      was ever created. */
}

void rrThreadSleep(uint32_t ms)
{
   struct timespec ts;
   ts.tv_sec  = ms / 1000;
   ts.tv_nsec = (long)(ms % 1000) * 1000000L;
   nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ *
 *  iggy_platform_deps                                                *
 * ------------------------------------------------------------------ */

/* Both take (context, arg) - the vendor bodies are literally
   `mov rdi,rsi; jmp malloc` and `mov rdi,rsi; jmp free`, i.e. the first parameter
   is a context pointer that is ignored and the *second* is the size/pointer. */
void *iggy_default_system_alloc(void *ctx, size_t size)
{
   (void)ctx;
   return malloc(size);
}

void iggy_default_system_free(void *ctx, void *ptr)
{
   (void)ctx;
   free(ptr);
}

/* Vendor body is a bare `ret`. */
void iggy_platform_init(void) { }

/* A U64 accumulator in .bss, NOT a function - IggyWaitOnFence brackets the GDraw
   wait with rrGetTicks and folds the delta in here. Getting this wrong (declaring it
   a function) is a link-time error, which is how it was caught. */
uint64_t iggy_fence_wait_ticks;

/* The vendor implementation calls gdraw_ps4_wait() on the fence and accounts the
   time. Our GDraw is the OpenGL backend, whose fence handling lives inside
   gdraw_gl_shared.inl, so there is nothing to wait on here - but keep the
   accounting so Iggy's own timing stats stay meaningful. */
void IggyWaitOnFence(void **fence, void *unused)
{
   uint64_t t0 = rrGetTicks();
   (void)fence; (void)unused;
   iggy_fence_wait_ticks += rrGetTicks() - t0;
}

/*
 * The packed date struct Iggy passes around. Field offsets and widths come straight
 * from iggy_gmtime_from_ms's stores (`mov WORD PTR [r15+N], ax`), and the year is
 * biased by 1900 (`mov r14d,0x76c` = 1900, added to tm_year).
 */
typedef struct IggyGmTime
{
   uint16_t ms;     /* +0x0 */
   uint16_t sec;    /* +0x2 */
   uint16_t min;    /* +0x4 */
   uint16_t hour;   /* +0x6 */
   uint16_t mday;   /* +0x8 */
   uint16_t mon;    /* +0xa  0-11, straight from tm_mon */
   uint16_t wday;   /* +0xc */
   uint16_t year;   /* +0xe  tm_year + 1900 */
} IggyGmTime;

/* Minutes that local time is offset from UTC, computed the way the vendor does it:
   mktime() of the UTC-decomposed current time, differenced against the real time. */
int iggy_get_timezone_offset_in_minutes(void)
{
   time_t now = time(NULL);
   struct tm g;
   if (!gmtime_r(&now, &g))
      return 0;
   g.tm_isdst = -1;
   return (int)(difftime(mktime(&g), now) / 60.0);
}

int64_t iggy_ms_gmtime(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000L);
}

void iggy_gmtime_from_ms(void *out, int64_t ms)
{
   IggyGmTime *o = (IggyGmTime *)out;
   struct tm g;
   time_t secs;
   int64_t q, rem;

   if (!o)
      return;

   /* Floor-divide, so the millisecond field is always in [0,999] and the second
      count stays consistent. The vendor truncates towards zero and then adds 1000 to
      a negative remainder without adjusting the seconds, which is off by one second
      for pre-1970 dates; this is the correction of that, and is identical for every
      non-negative timestamp. */
   q = ms / 1000;
   rem = ms - q * 1000;
   if (rem < 0) {
      rem += 1000;
      q   -= 1;
   }
   secs = (time_t)q;

   if (!gmtime_r(&secs, &g))
      memset(&g, 0, sizeof g);

   o->ms   = (uint16_t)rem;
   o->sec  = (uint16_t)g.tm_sec;
   o->min  = (uint16_t)g.tm_min;
   o->hour = (uint16_t)g.tm_hour;
   o->mday = (uint16_t)g.tm_mday;
   o->mon  = (uint16_t)g.tm_mon;
   o->wday = (uint16_t)g.tm_wday;
   o->year = (uint16_t)(g.tm_year + 1900);
}

int64_t iggy_ms_from_gmtime(void *in)
{
   const IggyGmTime *i = (const IggyGmTime *)in;
   struct tm t;
   time_t local;

   if (!i)
      return 0;

   memset(&t, 0, sizeof t);
   t.tm_sec   = i->sec;
   t.tm_min   = i->min;
   t.tm_hour  = i->hour;
   t.tm_mday  = i->mday;
   t.tm_mon   = i->mon;
   t.tm_year  = (int)i->year - 1900;
   t.tm_isdst = -1;

   /* mktime() interprets the fields as local time; the vendor corrects back to UTC
      with the same timezone delta iggy_get_timezone_offset_in_minutes computes. */
   local = mktime(&t);
   return ((int64_t)local + (int64_t)iggy_get_timezone_offset_in_minutes() * 60LL)
          * 1000LL + (int64_t)i->ms;
}

/* ------------------------------------------------------------------ *
 *  Sony libc internals (Dinkumware)                                  *
 * ------------------------------------------------------------------ */

/* FLT_ROUNDS. Referenced only by dtoa.obj. 1 == round to nearest. */
int _Fltrounds(void) { return 1; }

/*
 * _Log(x, base_flag). rrMath.obj's rrlog2 is `_Log(x, 0)` followed by a multiply by
 * the .rodata constant 0x3ff71547652b82fe == 1.4426950408889634 == 1/ln(2). So
 * _Log(x, 0) is the natural logarithm; a base-2 result would make that multiply
 * wrong. Only flag 0 is reached from this tree.
 */
double _Log(double x, int base_flag)
{
   return base_flag ? log2(x) : log(x);
}

/*
 * _Sin(x, quadrant_offset, ...). Every call site sets edi to 0 or 1 and esi to 0;
 * the 0/1 pairs appear together where sin and cos of one angle are both needed
 * (as3vm_DisplayObject_set_rotation, as3vm_Matrix_rotate/createBox,
 * iggy_get_displacement, the drop-shadow and bevel filters). So the offset is in
 * quarter-turns: 0 -> sin, 1 -> cos.
 */
double _Sin(double x, unsigned int quadrant_offset, unsigned int unused)
{
   (void)unused;
   return (quadrant_offset & 1u) ? cos(x) : sin(x);
}

/*
 * _Getpctype() returns Dinkumware's character-classification table. Call sites in
 * swf_draw_text.obj index it as `[table + c*2]` with c a full UTF-16 code unit and
 * test bit 0x20, immediately followed by `imul ecx,r12d,0xa` / `lea r12d,[rcx+rax-0x30]`
 * - decimal accumulation - so entries are 16-bit and 0x20 is Dinkumware's _DI
 * (digit) bit. The mask set below is Dinkumware's standard one.
 *
 * Two deliberate departures from a literal 256-entry port:
 *
 *  - The table is sized for the whole UTF-16 range. Iggy indexes it with an
 *    unbounded code unit, so a 256-entry table would be an out-of-bounds read on any
 *    non-ASCII character in a text field. Entries above 0x7f are left zero, which
 *    classifies them as "nothing in particular" - safe, and correct for the digit
 *    test that is actually performed.
 *  - Dinkumware's table is addressable from -128, and _Getpctype returns the pointer
 *    biased so a negative char indexes backwards. We keep that bias so a signed-char
 *    call site cannot read out of bounds either.
 */
#define _DI_XD  0x01u   /* hex digit   */
#define _DI_UP  0x02u   /* upper case  */
#define _DI_SP  0x04u   /* white space */
#define _DI_PU  0x08u   /* punctuation */
#define _DI_LO  0x10u   /* lower case  */
#define _DI_DI  0x20u   /* digit       */
#define _DI_CN  0x40u   /* control     */
#define _DI_BB  0x80u   /* blank       */

#define RAD_CTYPE_BIAS   128
#define RAD_CTYPE_COUNT  (RAD_CTYPE_BIAS + 0x10000)

static unsigned short rad_ctype_table[RAD_CTYPE_COUNT];
static int            rad_ctype_ready;

const unsigned short *_Getpctype(void)
{
   if (!rad_ctype_ready) {
      int c;
      for (c = 0; c < 0x80; ++c) {
         unsigned short m = 0;
         if (c >= '0' && c <= '9')                        m |= _DI_DI | _DI_XD;
         if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) m |= _DI_XD;
         if (c >= 'A' && c <= 'Z')                        m |= _DI_UP;
         if (c >= 'a' && c <= 'z')                        m |= _DI_LO;
         if (c == ' ')                                    m |= _DI_SP | _DI_BB;
         if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r')
                                                          m |= _DI_SP;
         if (c == '\t')                                   m |= _DI_BB;
         if (c < 0x20 || c == 0x7f)                       m |= _DI_CN;
         if (c > 0x20 && c < 0x7f && !(m & (_DI_DI | _DI_UP | _DI_LO)))
                                                          m |= _DI_PU;
         rad_ctype_table[RAD_CTYPE_BIAS + c] = m;
      }
      rrAtomicMemoryBarrierFull();
      rad_ctype_ready = 1;
   }
   return rad_ctype_table + RAD_CTYPE_BIAS;
}

/* ------------------------------------------------------------------ *
 *  Audio - never installed                                           *
 * ------------------------------------------------------------------ *
 *
 * Returning NULL means "no driver", which is what we want: the game drives its own
 * audio and Iggy's is a separate mixer for sound embedded in the SWFs. Windows64 is
 * the only platform that turns it on (IggyAudioUseDirectSound); Durango's
 * UIController has it commented out with "Iggy crashes if I have audio enabled".
 */
void *RADSS_SonyInstallDriver(void *ctx)
{
   (void)ctx;
   return NULL;
}
