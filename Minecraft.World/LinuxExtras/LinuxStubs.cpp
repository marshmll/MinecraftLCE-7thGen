#include "LinuxStubs.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <climits>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

// --- Critical sections -----------------------------------------------------

// Win32's CRITICAL_SECTION is always reentrant: a thread that already owns
// it can call EnterCriticalSection again without blocking itself, as long as
// it calls LeaveCriticalSection the same number of times. A plain
// pthread_mutex_init(..., NULL) gives a non-recursive (PTHREAD_MUTEX_DEFAULT)
// mutex - any code that recursively re-enters the same critical section on
// one thread (a valid, common pattern under real Win32) silently deadlocks
// instead. PTHREAD_MUTEX_RECURSIVE matches the real semantics being ported.
static pthread_mutexattr_t g_recursiveMutexAttr;
static bool g_recursiveMutexAttrInit = false;

static pthread_mutexattr_t *GetRecursiveMutexAttr()
{
    if (!g_recursiveMutexAttrInit)
    {
        pthread_mutexattr_init(&g_recursiveMutexAttr);
        pthread_mutexattr_settype(&g_recursiveMutexAttr, PTHREAD_MUTEX_RECURSIVE);
        g_recursiveMutexAttrInit = true;
    }
    return &g_recursiveMutexAttr;
}

void InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection)
{
    pthread_mutex_init(lpCriticalSection, GetRecursiveMutexAttr());
}

void InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION lpCriticalSection, ULONG /*SpinCount*/)
{
    pthread_mutex_init(lpCriticalSection, GetRecursiveMutexAttr());
}

void DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection)
{
    pthread_mutex_destroy(lpCriticalSection);
}

void EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection)
{
    pthread_mutex_lock(lpCriticalSection);
}

void LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection)
{
    pthread_mutex_unlock(lpCriticalSection);
}

ULONG TryEnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection)
{
    return pthread_mutex_trylock(lpCriticalSection) == 0 ? TRUE : FALSE;
}

// --- Events / waiting --------------------------------------------------------

// WaitForSingleObject/WaitForMultipleObjects are called on two structurally
// different HANDLE kinds under real Win32 - a CreateEvent() handle, and a
// CreateThread() handle (waiting for thread termination), e.g.
// C4JThread::WaitForCompletion() legitimately calls WaitForSingleObject on
// its own m_threadHandle. Both kinds are opaque `void*` at the call site, so
// a leading type tag (identical offset in both structs) lets the wait
// functions safely tell them apart instead of blindly reinterpreting
// whatever memory a thread handle points to as a LinuxEvent (which silently
// corrupted a pthread_mutex_t and deadlocked - see git history).
enum LinuxHandleKind { LINUX_HANDLE_EVENT, LINUX_HANDLE_THREAD };

struct LinuxEvent {
    LinuxHandleKind kind;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool manualReset;
    bool signaled;
};

// Defined here (rather than down by CreateThread/LinuxThreadTrampoline, its
// other use sites) so WaitForSingleObject below can see the full type and
// dispatch on `kind` instead of assuming every HANDLE is a LinuxEvent*.
struct LinuxThread {
    LinuxHandleKind kind;
    pthread_t thread;
    LPTHREAD_START_ROUTINE startAddress;
    LPVOID parameter;
    DWORD exitCode;
    bool finished;
    // Signaled by LinuxThreadTrampoline on exit, so WaitForSingleObject can
    // implement real Win32 "wait for thread termination" semantics
    // (including a timeout) rather than an all-or-nothing pthread_join.
    pthread_mutex_t completionMutex;
    pthread_cond_t completionCond;
};

HANDLE CreateEvent(void * /*lpEventAttributes*/, BOOL bManualReset, BOOL bInitialState, LPCSTR /*lpName*/)
{
    LinuxEvent *e = new LinuxEvent();
    e->kind = LINUX_HANDLE_EVENT;
    pthread_mutex_init(&e->mutex, NULL);
    pthread_cond_init(&e->cond, NULL);
    e->manualReset = bManualReset != FALSE;
    e->signaled = bInitialState != FALSE;
    return (HANDLE)e;
}

BOOL SetEvent(HANDLE hEvent)
{
    LinuxEvent *e = (LinuxEvent *)hEvent;
    pthread_mutex_lock(&e->mutex);
    e->signaled = true;
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->mutex);
    return TRUE;
}

BOOL ResetEvent(HANDLE hEvent)
{
    LinuxEvent *e = (LinuxEvent *)hEvent;
    pthread_mutex_lock(&e->mutex);
    e->signaled = false;
    pthread_mutex_unlock(&e->mutex);
    return TRUE;
}

BOOL CloseHandle(HANDLE hObject)
{
    // CreateFileA encodes file HANDLEs as small (fd+1) integers (see below),
    // while CreateEvent returns a real LinuxEvent* heap pointer - a single
    // CloseHandle (matching real Win32's one call for every handle kind)
    // has to tell them apart. Real heap/mmap pointers on 64-bit Linux are
    // always far above any process's fd table size, so this threshold is
    // safe in practice.
    intptr_t asInt = (intptr_t)hObject;
    if (asInt > 0 && asInt < 0x10000)
    {
        int fd = (int)asInt - 1;
        return close(fd) == 0 ? TRUE : FALSE;
    }

    LinuxEvent *e = (LinuxEvent *)hObject;
    pthread_mutex_destroy(&e->mutex);
    pthread_cond_destroy(&e->cond);
    delete e;
    return TRUE;
}

VOID Sleep(DWORD dwMilliseconds)
{
    struct timespec ts;
    ts.tv_sec = dwMilliseconds / 1000;
    ts.tv_nsec = (dwMilliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

BOOL SetThreadPriority(HANDLE /*hThread*/, int /*nPriority*/)
{
    return TRUE;
}

static DWORD WaitEventWithTimeout(LinuxEvent *e, DWORD dwMilliseconds)
{
    pthread_mutex_lock(&e->mutex);
    DWORD result = WAIT_OBJECT_0;
    if (!e->signaled) {
        if (dwMilliseconds == INFINITE) {
            while (!e->signaled)
                pthread_cond_wait(&e->cond, &e->mutex);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += dwMilliseconds / 1000;
            ts.tv_nsec += (dwMilliseconds % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
            int rc = 0;
            while (!e->signaled && rc == 0)
                rc = pthread_cond_timedwait(&e->cond, &e->mutex, &ts);
            if (!e->signaled)
                result = WAIT_TIMEOUT;
        }
    }
    if (result == WAIT_OBJECT_0 && !e->manualReset)
        e->signaled = false;
    pthread_mutex_unlock(&e->mutex);
    return result;
}

static DWORD WaitThreadWithTimeout(LinuxThread *t, DWORD dwMilliseconds)
{
    pthread_mutex_lock(&t->completionMutex);
    DWORD result = WAIT_OBJECT_0;
    if (!t->finished) {
        if (dwMilliseconds == INFINITE) {
            while (!t->finished)
                pthread_cond_wait(&t->completionCond, &t->completionMutex);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += dwMilliseconds / 1000;
            ts.tv_nsec += (dwMilliseconds % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
            int rc = 0;
            while (!t->finished && rc == 0)
                rc = pthread_cond_timedwait(&t->completionCond, &t->completionMutex, &ts);
            if (!t->finished)
                result = WAIT_TIMEOUT;
        }
    }
    pthread_mutex_unlock(&t->completionMutex);
    return result;
}

DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    // Small-int fd-encoded HANDLEs (see CreateFileA/CloseHandle) are never
    // waited on - only real LinuxEvent*/LinuxThread* pointers reach here.
    LinuxHandleKind kind = *(LinuxHandleKind *)hHandle;
    if (kind == LINUX_HANDLE_THREAD)
        return WaitThreadWithTimeout((LinuxThread *)hHandle, dwMilliseconds);
    return WaitEventWithTimeout((LinuxEvent *)hHandle, dwMilliseconds);
}

DWORD WaitForMultipleObjects(DWORD nCount, HANDLE const *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds)
{
    if (!bWaitAll) {
        for (DWORD i = 0; i < nCount; ++i)
            if (WaitForSingleObject(lpHandles[i], 0) == WAIT_OBJECT_0)
                return WAIT_OBJECT_0 + i;
        if (dwMilliseconds == 0)
            return WAIT_TIMEOUT;
        return WaitForSingleObject(lpHandles[0], dwMilliseconds);
    }
    for (DWORD i = 0; i < nCount; ++i)
        if (WaitForSingleObject(lpHandles[i], dwMilliseconds) != WAIT_OBJECT_0)
            return WAIT_TIMEOUT;
    return WAIT_OBJECT_0;
}

DWORD WaitForMultipleObjectsEx(DWORD nCount, HANDLE const *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds, BOOL /*bAlertable*/)
{
    return WaitForMultipleObjects(nCount, lpHandles, bWaitAll, dwMilliseconds);
}

LONG InterlockedCompareExchangeRelease(LONG volatile *Destination, LONG Exchange, LONG Comperand)
{
    __sync_synchronize();
    return __sync_val_compare_and_swap(Destination, Comperand, Exchange);
}

LONG64 InterlockedCompareExchangeRelease64(LONG64 volatile *Destination, LONG64 Exchange, LONG64 Comperand)
{
    __sync_synchronize();
    return __sync_val_compare_and_swap(Destination, Comperand, Exchange);
}

HMODULE GetModuleHandle(LPCSTR /*lpModuleName*/)
{
    return NULL;
}

// --- Threads -----------------------------------------------------------------

// LinuxThread's full definition now lives up by LinuxEvent's (both need a
// shared leading `kind` tag for WaitForSingleObject to dispatch on).

static void *LinuxThreadTrampoline(void *arg)
{
    LinuxThread *t = (LinuxThread *)arg;
    t->exitCode = t->startAddress(t->parameter);
    pthread_mutex_lock(&t->completionMutex);
    t->finished = true;
    pthread_cond_broadcast(&t->completionCond);
    pthread_mutex_unlock(&t->completionMutex);
    return NULL;
}

HANDLE CreateThread(void * /*lpThreadAttributes*/, DWORD /*dwStackSize*/, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId)
{
    LinuxThread *t = new LinuxThread();
    t->kind = LINUX_HANDLE_THREAD;
    t->startAddress = lpStartAddress;
    t->parameter = lpParameter;
    t->exitCode = 0;
    t->finished = false;
    pthread_mutex_init(&t->completionMutex, NULL);
    pthread_cond_init(&t->completionCond, NULL);

    if (dwCreationFlags & CREATE_SUSPENDED) {
        // Suspended-at-creation isn't modeled; the thread starts immediately
        // on ResumeThread instead, which covers how the game code uses this.
        return (HANDLE)t;
    }

    pthread_create(&t->thread, NULL, LinuxThreadTrampoline, t);
    if (lpThreadId)
        *lpThreadId = (DWORD)(uintptr_t)t->thread;
    return (HANDLE)t;
}

DWORD ResumeThread(HANDLE hThread)
{
    LinuxThread *t = (LinuxThread *)hThread;
    pthread_create(&t->thread, NULL, LinuxThreadTrampoline, t);
    return 0;
}

DWORD GetCurrentThreadId(VOID)
{
    return (DWORD)(uintptr_t)pthread_self();
}

HANDLE GetCurrentThread(VOID)
{
    // Real Windows returns a constant pseudo-handle here too (-2), valid
    // only for operations on the calling thread itself.
    return (HANDLE)(intptr_t)-2;
}

BOOL GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode)
{
    LinuxThread *t = (LinuxThread *)hThread;
    *lpExitCode = t->finished ? t->exitCode : (DWORD)STILL_ACTIVE;
    return TRUE;
}

// --- Thread-local storage -----------------------------------------------------

DWORD TlsAlloc(VOID)
{
    pthread_key_t key;
    pthread_key_create(&key, NULL);
    return (DWORD)key;
}

LPVOID TlsGetValue(DWORD dwTlsIndex)
{
    return pthread_getspecific((pthread_key_t)dwTlsIndex);
}

BOOL TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue)
{
    return pthread_setspecific((pthread_key_t)dwTlsIndex, lpTlsValue) == 0 ? TRUE : FALSE;
}

// --- Virtual memory -----------------------------------------------------------

LPVOID VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD /*flAllocationType*/, DWORD /*flProtect*/)
{
    void *p = mmap(lpAddress, dwSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

BOOL VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD /*dwFreeType*/)
{
    return munmap(lpAddress, dwSize) == 0 ? TRUE : FALSE;
}

// --- File I/O -------------------------------------------------------------

HANDLE CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD /*dwShareMode*/, LPSECURITY_ATTRIBUTES /*lpSecurityAttributes*/, DWORD dwCreationDisposition, DWORD /*dwFlagsAndAttributes*/, HANDLE /*hTemplateFile*/)
{
    int flags = 0;
    bool wantRead = (dwDesiredAccess & GENERIC_READ) != 0;
    bool wantWrite = (dwDesiredAccess & GENERIC_WRITE) != 0;
    if (wantRead && wantWrite) flags = O_RDWR;
    else if (wantWrite) flags = O_WRONLY;
    else flags = O_RDONLY;

    switch (dwCreationDisposition) {
        case CREATE_NEW:       flags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS:    flags |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS:      flags |= O_CREAT; break;
        case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
        case OPEN_EXISTING: default: break;
    }

    int fd = open(lpFileName, flags, 0644);
    if (fd < 0)
        return INVALID_HANDLE_VALUE;
    return (HANDLE)(intptr_t)(fd + 1); // +1 so fd 0 doesn't collide with a NULL/0 handle
}

static int HandleToFd(HANDLE hFile)
{
    return (int)(intptr_t)hFile - 1;
}

BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED /*lpOverlapped*/)
{
    ssize_t n = read(HandleToFd(hFile), lpBuffer, nNumberOfBytesToRead);
    if (n < 0) {
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
        return FALSE;
    }
    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = (DWORD)n;
    return TRUE;
}

BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED /*lpOverlapped*/)
{
    ssize_t n = write(HandleToFd(hFile), lpBuffer, nNumberOfBytesToWrite);
    if (n < 0) {
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
        return FALSE;
    }
    if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = (DWORD)n;
    return TRUE;
}

BOOL SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod)
{
    int whence = dwMoveMethod == FILE_BEGIN ? SEEK_SET : dwMoveMethod == FILE_END ? SEEK_END : SEEK_CUR;
    off_t r = lseek(HandleToFd(hFile), lDistanceToMove, whence);
    if (lpDistanceToMoveHigh) *lpDistanceToMoveHigh = 0;
    return r != (off_t)-1 ? TRUE : FALSE;
}

DWORD GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh)
{
    struct stat st;
    if (fstat(HandleToFd(hFile), &st) != 0)
        return (DWORD)-1;
    if (lpFileSizeHigh) *lpFileSizeHigh = (DWORD)(st.st_size >> 32);
    return (DWORD)(st.st_size & 0xFFFFFFFFu);
}

BOOL GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize)
{
    struct stat st;
    if (fstat(HandleToFd(hFile), &st) != 0)
        return FALSE;
    lpFileSize->QuadPart = st.st_size;
    return TRUE;
}

BOOL CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES /*lpSecurityAttributes*/)
{
    return mkdir(lpPathName, 0755) == 0 || errno == EEXIST ? TRUE : FALSE;
}

BOOL DeleteFileA(LPCSTR lpFileName)
{
    return unlink(lpFileName) == 0 ? TRUE : FALSE;
}

DWORD GetFileAttributesA(LPCSTR lpFileName)
{
    struct stat st;
    if (stat(lpFileName, &st) != 0)
        return (DWORD)-1; // INVALID_FILE_ATTRIBUTES
    return (st.st_mode & S_IFDIR) ? (DWORD)FILE_ATTRIBUTE_DIRECTORY : (DWORD)FILE_ATTRIBUTE_NORMAL;
}

BOOL GetFileAttributesExA(LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS /*fInfoLevelId*/, LPVOID lpFileInformation)
{
    struct stat st;
    if (stat(lpFileName, &st) != 0)
        return FALSE;
    WIN32_FILE_ATTRIBUTE_DATA *data = (WIN32_FILE_ATTRIBUTE_DATA *)lpFileInformation;
    data->dwFileAttributes = (st.st_mode & S_IFDIR) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    data->nFileSizeHigh = (DWORD)(st.st_size >> 32);
    data->nFileSizeLow = (DWORD)(st.st_size & 0xFFFFFFFFu);
    return TRUE;
}

BOOL MoveFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName)
{
    return rename(lpExistingFileName, lpNewFileName) == 0 ? TRUE : FALSE;
}

HANDLE FindFirstFileA(LPCSTR /*lpFileName*/, LPWIN32_FIND_DATA /*lpFindFileData*/)
{
    // Not exercised by the World portability path today; revisit if a
    // real caller shows up while bringing up the Linux client.
    LINUX_STUBBED
    return INVALID_HANDLE_VALUE;
}

BOOL FindNextFileA(HANDLE /*hFindFile*/, LPWIN32_FIND_DATAA /*lpFindFileData*/)
{
    return FALSE;
}

// --- Time -----------------------------------------------------------------

// wMilliseconds MUST be filled in. System::currentTimeMillis() (Minecraft.World/
// system.cpp:66) is built out of GetSystemTime() + SystemTimeToFileTime(), and it
// drives the game/server tick loops. Returning whole seconds made that clock stand
// still for a second and then jump 1000ms, so a second's worth of ticks fired in one
// burst and then nothing happened until the next second - which is what "mob movement
// updates once a second, very spiky" was.
VOID GetSystemTime(LPSYSTEMTIME lpSystemTime)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t now = (time_t)ts.tv_sec;
    struct tm tmv;
    gmtime_r(&now, &tmv);
    lpSystemTime->wYear = (WORD)(tmv.tm_year + 1900);
    lpSystemTime->wMonth = (WORD)(tmv.tm_mon + 1);
    lpSystemTime->wDayOfWeek = (WORD)tmv.tm_wday;
    lpSystemTime->wDay = (WORD)tmv.tm_mday;
    lpSystemTime->wHour = (WORD)tmv.tm_hour;
    lpSystemTime->wMinute = (WORD)tmv.tm_min;
    lpSystemTime->wSecond = (WORD)tmv.tm_sec;
    lpSystemTime->wMilliseconds = (WORD)(ts.tv_nsec / 1000000L);
}

VOID GetLocalTime(LPSYSTEMTIME lpSystemTime)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t now = (time_t)ts.tv_sec;
    struct tm tmv;
    localtime_r(&now, &tmv);
    lpSystemTime->wYear = (WORD)(tmv.tm_year + 1900);
    lpSystemTime->wMonth = (WORD)(tmv.tm_mon + 1);
    lpSystemTime->wDayOfWeek = (WORD)tmv.tm_wday;
    lpSystemTime->wDay = (WORD)tmv.tm_mday;
    lpSystemTime->wHour = (WORD)tmv.tm_hour;
    lpSystemTime->wMinute = (WORD)tmv.tm_min;
    lpSystemTime->wSecond = (WORD)tmv.tm_sec;
    lpSystemTime->wMilliseconds = (WORD)(ts.tv_nsec / 1000000L);
}

BOOL FileTimeToSystemTime(CONST FILETIME *lpFileTime, LPSYSTEMTIME lpSystemTime)
{
    ULONGLONG ticks = ((ULONGLONG)lpFileTime->dwHighDateTime << 32) | lpFileTime->dwLowDateTime;
    ULONGLONG totalMs = ticks / 10000ULL;
    time_t seconds = (time_t)(totalMs / 1000ULL) - 11644473600LL;
    struct tm tmv;
    gmtime_r(&seconds, &tmv);
    lpSystemTime->wYear = (WORD)(tmv.tm_year + 1900);
    lpSystemTime->wMonth = (WORD)(tmv.tm_mon + 1);
    lpSystemTime->wDayOfWeek = (WORD)tmv.tm_wday;
    lpSystemTime->wDay = (WORD)tmv.tm_mday;
    lpSystemTime->wHour = (WORD)tmv.tm_hour;
    lpSystemTime->wMinute = (WORD)tmv.tm_min;
    lpSystemTime->wSecond = (WORD)tmv.tm_sec;
    lpSystemTime->wMilliseconds = (WORD)(totalMs % 1000ULL);
    return TRUE;
}

BOOL SystemTimeToFileTime(CONST SYSTEMTIME *lpSystemTime, LPFILETIME lpFileTime)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = lpSystemTime->wYear - 1900;
    tmv.tm_mon = lpSystemTime->wMonth - 1;
    tmv.tm_mday = lpSystemTime->wDay;
    tmv.tm_hour = lpSystemTime->wHour;
    tmv.tm_min = lpSystemTime->wMinute;
    tmv.tm_sec = lpSystemTime->wSecond;
    time_t seconds = timegm(&tmv);
    // Include wMilliseconds: dropping it here would throw away the sub-second part
    // of GetSystemTime() again and re-break System::currentTimeMillis().
    ULONGLONG ticks = ((ULONGLONG)seconds + 11644473600ULL) * 10000000ULL
                      + (ULONGLONG)lpSystemTime->wMilliseconds * 10000ULL;
    lpFileTime->dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFu);
    lpFileTime->dwHighDateTime = (DWORD)(ticks >> 32);
    return TRUE;
}

DWORD GetTickCount()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency)
{
    lpFrequency->QuadPart = 1000000000LL; // clock_gettime resolution: nanoseconds
    return TRUE;
}

BOOL QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    lpPerformanceCount->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}

// --- Misc -------------------------------------------------------------------

void __debugbreak() {}
VOID DebugBreak(VOID) {}

DWORD GetLastError(VOID)
{
    return (DWORD)errno;
}

VOID GlobalMemoryStatus(LPMEMORYSTATUS lpBuffer)
{
    long pages = sysconf(_SC_PHYS_PAGES);
    long avPages = sysconf(_SC_AVPHYS_PAGES);
    long pageSize = sysconf(_SC_PAGE_SIZE);
    lpBuffer->dwLength = sizeof(MEMORYSTATUS);
    lpBuffer->dwMemoryLoad = 0;
    lpBuffer->dwTotalPhys = (SIZE_T)pages * pageSize;
    lpBuffer->dwAvailPhys = (SIZE_T)avPages * pageSize;
    lpBuffer->dwTotalPageFile = lpBuffer->dwTotalPhys;
    lpBuffer->dwAvailPageFile = lpBuffer->dwAvailPhys;
    lpBuffer->dwTotalVirtual = lpBuffer->dwTotalPhys;
    lpBuffer->dwAvailVirtual = lpBuffer->dwAvailPhys;
}

VOID OutputDebugStringW(LPCWSTR lpOutputString)
{
    fwprintf(stderr, L"%ls", lpOutputString);
}

VOID OutputDebugStringA(LPCSTR lpOutputString)
{
    fputs(lpOutputString, stderr);
}

VOID OutputDebugString(LPCSTR lpOutputString)
{
    OutputDebugStringA(lpOutputString);
}

errno_t _itoa_s(int _Value, char *_DstBuf, size_t _Size, int _Radix)
{
    if (_Radix == 10)
        snprintf(_DstBuf, _Size, "%d", _Value);
    else if (_Radix == 16)
        snprintf(_DstBuf, _Size, "%x", _Value);
    else
        return -1;
    return 0;
}

errno_t _i64toa_s(__int64 _Val, char *_DstBuf, size_t _Size, int _Radix)
{
    if (_Radix == 10)
        snprintf(_DstBuf, _Size, "%lld", (long long)_Val);
    else if (_Radix == 16)
        snprintf(_DstBuf, _Size, "%llx", (long long)_Val);
    else
        return -1;
    return 0;
}

int _wtoi(const wchar_t *_Str)
{
    return (int)wcstol(_Str, NULL, 10);
}

int sprintf_s(char *_DstBuf, size_t _Size, const char *_Format, ...)
{
    va_list args;
    va_start(args, _Format);
    int n = vsnprintf(_DstBuf, _Size, _Format, args);
    va_end(args);
    return n;
}

int swscanf_s(const wchar_t *_Buffer, const wchar_t *_Format, ...)
{
    // The "_s" suffix on MSVC's CRT only changes behavior for %s/%c
    // specifiers (which need extra buffer-size args); call sites in this
    // codebase only use it with %d, so a plain vswscanf is equivalent.
    va_list args;
    va_start(args, _Format);
    int n = vswscanf(_Buffer, _Format, args);
    va_end(args);
    return n;
}

extern "C" int _wcsicmp(const wchar_t *dst, const wchar_t *src)
{
    return wcscasecmp(dst, src);
}

wchar_t *_itow(int _Value, wchar_t *_DstBuf, int _Radix)
{
    static const wchar_t digits[] = L"0123456789abcdefghijklmnopqrstuvwxyz";
    wchar_t buf[34];
    wchar_t *p = buf + 33;
    *p = L'\0';

    bool negative = _Value < 0 && _Radix == 10;
    unsigned int v = negative ? (unsigned int)(-_Value) : (unsigned int)_Value;

    do {
        *--p = digits[v % _Radix];
        v /= _Radix;
    } while (v != 0);

    if (negative)
        *--p = L'-';

    wcscpy(_DstBuf, p);
    return _DstBuf;
}
