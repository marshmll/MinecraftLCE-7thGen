#pragma once

// Win32-API compatibility shim for the Linux target, mirroring the role of
// Minecraft.Client/PS3/PS3Extras/Ps3Stubs.h (and the Orbis/PSVita equivalents):
// shared World/Client code calls these Win32 primitives unconditionally, and
// each non-Windows platform maps them onto its own native APIs. Here that's
// pthreads/POSIX. Definitions are in LinuxStubs.cpp.

#include "LinuxTypes.h"
#include <pthread.h>
#include <cstdarg>
#include <cstdio>

#ifndef _CONTENT_PACKAGE
#define DEBUG_PRINTF(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINTF(fmt, ...) (void)0
#endif

typedef struct _RECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *PRECT;

// 4J_Render.h is written against D3D11 - on non-D3D platforms these are
// opaque/unused, exactly as Ps3Stubs.h/OrbisStubs.h treat them.
typedef void ID3D11Device;
typedef void ID3D11DeviceContext;
typedef void IDXGISwapChain;
typedef RECT D3D11_RECT;
typedef void ID3D11Buffer;
typedef void ID3D11ShaderResourceView;

typedef DWORD (*PTHREAD_START_ROUTINE)(LPVOID lpThreadParameter);
typedef PTHREAD_START_ROUTINE LPTHREAD_START_ROUTINE;

typedef int errno_t;

#define S_OK 0

typedef pthread_mutex_t CRITICAL_SECTION;
typedef pthread_mutex_t *PCRITICAL_SECTION, *LPCRITICAL_SECTION;

void InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION lpCriticalSection, ULONG SpinCount);
void DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
ULONG TryEnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);

HANDLE CreateEvent(void *lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName);
BOOL SetEvent(HANDLE hEvent);
BOOL ResetEvent(HANDLE hEvent);
BOOL CloseHandle(HANDLE hObject);
VOID Sleep(DWORD dwMilliseconds);
BOOL SetThreadPriority(HANDLE hThread, int nPriority);
DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
DWORD WaitForMultipleObjects(DWORD nCount, HANDLE const *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds);
DWORD WaitForMultipleObjectsEx(DWORD nCount, HANDLE const *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds, BOOL bAlertable);

LONG InterlockedCompareExchangeRelease(LONG volatile *Destination, LONG Exchange, LONG Comperand);
LONG64 InterlockedCompareExchangeRelease64(LONG64 volatile *Destination, LONG64 Exchange, LONG64 Comperand);

HMODULE GetModuleHandle(LPCSTR lpModuleName);

HANDLE CreateThread(void *lpThreadAttributes, DWORD dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId);
DWORD ResumeThread(HANDLE hThread);
DWORD GetCurrentThreadId(VOID);
HANDLE GetCurrentThread(VOID);
BOOL GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode);

DWORD TlsAlloc(VOID);
LPVOID TlsGetValue(DWORD dwTlsIndex);
BOOL TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue);

LPVOID VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
BOOL VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);

DWORD GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
BOOL GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize);
BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped);
#define INVALID_SET_FILE_POINTER false
BOOL SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod);
HANDLE CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
#define CreateFile CreateFileA
BOOL CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
#define CreateDirectory CreateDirectoryA
BOOL DeleteFileA(LPCSTR lpFileName);
#define DeleteFile DeleteFileA
DWORD GetFileAttributesA(LPCSTR lpFileName);
#define GetFileAttributes GetFileAttributesA
BOOL MoveFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName);
#define MoveFile MoveFileA

#define MAX_PATH 260

void __debugbreak();
VOID DebugBreak(VOID);

enum D3D11_BLEND {
    D3D11_BLEND_ZERO = 1,
    D3D11_BLEND_ONE = 2,
    D3D11_BLEND_SRC_COLOR = 3,
    D3D11_BLEND_INV_SRC_COLOR = 4,
    D3D11_BLEND_SRC_ALPHA = 5,
    D3D11_BLEND_INV_SRC_ALPHA = 6,
    D3D11_BLEND_DEST_ALPHA = 7,
    D3D11_BLEND_INV_DEST_ALPHA = 8,
    D3D11_BLEND_DEST_COLOR = 9,
    D3D11_BLEND_INV_DEST_COLOR = 10,
    D3D11_BLEND_SRC_ALPHA_SAT = 11,
    D3D11_BLEND_BLEND_FACTOR = 14,
    D3D11_BLEND_INV_BLEND_FACTOR = 15,
    D3D11_BLEND_SRC1_COLOR = 16,
    D3D11_BLEND_INV_SRC1_COLOR = 17,
    D3D11_BLEND_SRC1_ALPHA = 18,
    D3D11_BLEND_INV_SRC1_ALPHA = 19
};

enum D3D11_COMPARISON_FUNC {
    D3D11_COMPARISON_NEVER = 1,
    D3D11_COMPARISON_LESS = 2,
    D3D11_COMPARISON_EQUAL = 3,
    D3D11_COMPARISON_LESS_EQUAL = 4,
    D3D11_COMPARISON_GREATER = 5,
    D3D11_COMPARISON_NOT_EQUAL = 6,
    D3D11_COMPARISON_GREATER_EQUAL = 7,
    D3D11_COMPARISON_ALWAYS = 8
};

typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

VOID GetSystemTime(LPSYSTEMTIME lpSystemTime);
BOOL FileTimeToSystemTime(CONST FILETIME *lpFileTime, LPSYSTEMTIME lpSystemTime);
BOOL SystemTimeToFileTime(CONST SYSTEMTIME *lpSystemTime, LPFILETIME lpFileTime);
VOID GetLocalTime(LPSYSTEMTIME lpSystemTime);

typedef struct _MEMORYSTATUS {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    SIZE_T dwTotalPhys;
    SIZE_T dwAvailPhys;
    SIZE_T dwTotalPageFile;
    SIZE_T dwAvailPageFile;
    SIZE_T dwTotalVirtual;
    SIZE_T dwAvailVirtual;
} MEMORYSTATUS, *LPMEMORYSTATUS;

#define WINAPI
#define CREATE_SUSPENDED 0x00000004

#define THREAD_BASE_PRIORITY_LOWRT 15
#define THREAD_BASE_PRIORITY_MAX 2
#define THREAD_BASE_PRIORITY_MIN -2
#define THREAD_BASE_PRIORITY_IDLE -15
#define THREAD_PRIORITY_LOWEST THREAD_BASE_PRIORITY_MIN
#define THREAD_PRIORITY_BELOW_NORMAL (THREAD_PRIORITY_LOWEST+1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_HIGHEST THREAD_BASE_PRIORITY_MAX
#define THREAD_PRIORITY_ABOVE_NORMAL (THREAD_PRIORITY_HIGHEST-1)
#define THREAD_PRIORITY_ERROR_RETURN (MAXLONG)
#define THREAD_PRIORITY_TIME_CRITICAL THREAD_BASE_PRIORITY_LOWRT
#define THREAD_PRIORITY_IDLE THREAD_BASE_PRIORITY_IDLE

#define WAIT_TIMEOUT 258L
#define STATUS_ABANDONED_WAIT_0 ((DWORD)0x00000080L)
#define WAIT_ABANDONED (STATUS_ABANDONED_WAIT_0 + 0)

#define MAXUINT_PTR (~((UINT_PTR)0))
#define MAXINT_PTR ((INT_PTR)(MAXUINT_PTR >> 1))
#define MININT_PTR (~MAXINT_PTR)
#define MAXULONG_PTR (~((ULONG_PTR)0))
#define MAXLONG_PTR ((LONG_PTR)(MAXULONG_PTR >> 1))
#define MINLONG_PTR (~MAXLONG_PTR)

#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define HRESULT_SUCCEEDED(Status) ((HRESULT)(Status) >= 0)
#define FAILED(Status) ((HRESULT)(Status)<0)

#define GENERIC_READ (0x80000000L)
#define GENERIC_WRITE (0x40000000L)
#define GENERIC_EXECUTE (0x20000000L)
#define GENERIC_ALL (0x10000000L)

#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_DEVICE 0x00000040
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100

#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000

#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2

#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5

#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100
#define PAGE_NOCACHE 0x200
#define PAGE_WRITECOMBINE 0x400
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define MEM_FREE 0x10000
#define MEM_PRIVATE 0x20000
#define MEM_RESET 0x80000
#define MEM_TOP_DOWN 0x100000
#define MEM_LARGE_PAGES 0x20000000

#define IGNORE 0
#define INFINITE 0xFFFFFFFF
#define WAIT_FAILED ((DWORD)0xFFFFFFFF)
#define STATUS_WAIT_0 ((DWORD)0x00000000L)
#define WAIT_OBJECT_0 (STATUS_WAIT_0 + 0)
#define STATUS_PENDING ((DWORD)0x00000103L)
#define STILL_ACTIVE STATUS_PENDING

DWORD GetLastError(VOID);
VOID GlobalMemoryStatus(LPMEMORYSTATUS lpBuffer);

DWORD GetTickCount();
BOOL QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency);
BOOL QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount);

#define ERROR_SUCCESS 0L
#define ERROR_IO_PENDING 997L
#define ERROR_CANCELLED 1223L
#define S_FALSE ((HRESULT)0x00000001L)
#define E_FAIL ((HRESULT)0x80004005L)

#define RtlEqualMemory(Destination,Source,Length) (!memcmp((Destination),(Source),(Length)))
#define RtlMoveMemory(Destination,Source,Length) memmove((Destination),(Source),(Length))
#define RtlCopyMemory(Destination,Source,Length) memcpy((Destination),(Source),(Length))
#define RtlFillMemory(Destination,Length,Fill) memset((Destination),(Fill),(Length))
#define RtlZeroMemory(Destination,Length) memset((Destination),0,(Length))
#define MoveMemory RtlMoveMemory
#define CopyMemory RtlCopyMemory
#define FillMemory RtlFillMemory
#define ZeroMemory RtlZeroMemory

#define CDECL
#define APIENTRY
#define wsprintfW swprintf
#define wcsncpy_s(dst, dstsz, src, cnt) wcsncpy((dst), (src), (cnt))
#define _wcstoui64 wcstoull

#define VK_ESCAPE 0x1B
#define VK_RETURN 0x0D

VOID OutputDebugStringW(LPCWSTR lpOutputString);
VOID OutputDebugString(LPCSTR lpOutputString);
VOID OutputDebugStringA(LPCSTR lpOutputString);

errno_t _itoa_s(int _Value, char *_DstBuf, size_t _Size, int _Radix);
errno_t _i64toa_s(__int64 _Val, char *_DstBuf, size_t _Size, int _Radix);
int _wtoi(const wchar_t *_Str);
wchar_t *_itow(int _Value, wchar_t *_DstBuf, int _Radix);
int sprintf_s(char *_DstBuf, size_t _Size, const char *_Format, ...);
int swscanf_s(const wchar_t *_Buffer, const wchar_t *_Format, ...);

#define _TRUNCATE ((size_t)-1)

// MSVC's secure CRT has an array-deducing template overload of
// _vsnprintf_s(buffer, count, format, argptr) that infers the destination
// size from a fixed-size array argument (e.g. `char str[2048]`), which is
// exactly how Console_Utils.cpp's DebugSpewV() calls it (4 args, no
// explicit buffer-size param). Mirrored here the same way, rather than as
// a plain function, since there's no other way to recover the real buffer
// size from a 4-arg call.
// VaListT is templated (rather than plain va_list) because callers sometimes
// pass a `const va_list` local - glibc's va_list is itself an array/pointer
// typedef, so const-qualification produces a type (e.g. `const
// __va_list_tag*`) that doesn't bind to a plain `va_list` parameter. vsnprintf
// itself is fine reading through either.
template <size_t _Size>
inline int _vsnprintf_s(char (&_DstBuf)[_Size], size_t _MaxCount, const char *_Format, va_list _ArgList)
{
	size_t n = (_MaxCount == _TRUNCATE || _MaxCount > _Size) ? _Size : _MaxCount;
	return vsnprintf(_DstBuf, n, _Format, _ArgList);
}

#define __declspec(a)
extern "C" int _wcsicmp(const wchar_t *dst, const wchar_t *src);

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *PWIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef WIN32_FIND_DATAA WIN32_FIND_DATA;
typedef PWIN32_FIND_DATAA PWIN32_FIND_DATA;
typedef LPWIN32_FIND_DATAA LPWIN32_FIND_DATA;

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;

typedef enum _GET_FILEEX_INFO_LEVELS {
    GetFileExInfoStandard,
    GetFileExMaxInfoLevel
} GET_FILEEX_INFO_LEVELS;

BOOL GetFileAttributesExA(LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);
#define GetFileAttributesEx GetFileAttributesExA

HANDLE FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATA lpFindFileData);
#define FindFirstFile FindFirstFileA
BOOL FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
#define FindNextFile FindNextFileA
#define FindClose(hFindFile) CloseHandle(hFindFile)

#define LINUX_STUBBED { static bool bSet = false; if(!bSet){printf("missing function on Linux : %s\n", __FUNCTION__); bSet = true;} }
