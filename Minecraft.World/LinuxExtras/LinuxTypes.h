#pragma once

// Win32 primitive typedefs used unconditionally by Minecraft.World/Minecraft.Client
// shared code. Mirrors the role of Ps3Types.h/OrbisTypes.h/PSVitaTypes.h for the
// Linux target. Unlike those (2010-2013 era compilers), a modern Linux toolchain
// already has a real C++11+ standard library, so none of the boost::tr1 /
// std::tr1 shimming those files need applies here - this file is just the
// Win32 type vocabulary the shared code expects to exist.

#include <cstddef>
#include <cstdint>
#include <cwchar>

// Win32's DWORD is 32-bit on every Windows target - `unsigned long` is 32-bit under
// LLP64, but 64-bit under the LP64 model Linux uses, so spelling it that way here made
// every DWORD twice the size the shared code assumes.
//
// That is not merely a size difference: the shared code type-puns through it. The one
// that surfaced was UIScene_FullscreenProgress::tick():
//
//     int code = thread->GetExitCode();      // 4 bytes
//     DWORD exitcode = *((DWORD *)&code);    // read 8 bytes from a 4-byte object
//
// The upper 4 bytes came from adjacent stack, so `exitcode != STILL_ACTIVE` was true even
// when the low 32 bits held exactly STILL_ACTIVE (0x103) - the progress screen concluded
// that the world-launch and exit-to-menu threads had *failed* and raised
// "Connection to the server was lost. Exiting to the main menu."
//
// It also silently corrupted every DWORD passed through varargs (DebugPrintf's "%d"/"%x"),
// every LPDWORD out-param (GetExitCodeThread wrote 8 bytes through one), and the layout of
// every struct with a DWORD member - including the save-file and profile blobs, which are
// now the same size the MSVC builds produce.
typedef unsigned int        DWORD;
typedef int                 BOOL;
typedef unsigned char       BYTE;
typedef unsigned short      WORD;
typedef float                FLOAT;
typedef int                 INT;
typedef unsigned int        UINT;
typedef unsigned char       byte;
typedef int                  __int32;
typedef long long            __int64;
typedef unsigned long long   __uint64;

typedef FLOAT   *PFLOAT;
typedef BOOL    *PBOOL, *LPBOOL;
typedef BYTE    *PBYTE, *LPBYTE;
typedef int     *PINT, *LPINT;
typedef WORD    *PWORD, *LPWORD;
typedef DWORD   *PDWORD, *LPDWORD;
typedef void    *PVOID, *LPVOID;
typedef const void *LPCVOID;
typedef unsigned int *PUINT;

// 32-bit on Windows, for the same LLP64-vs-LP64 reason as DWORD above.
typedef unsigned int ULONG;
typedef unsigned char boolean;
// ULONG_PTR/SIZE_T *are* pointer-sized, so `unsigned long` is right for these two.
typedef unsigned long ULONG_PTR, *PULONG_PTR;
typedef ULONG_PTR SIZE_T, *PSIZE_T;

#define VOID void
typedef char CHAR;
typedef short SHORT;
// Also 32-bit on Windows. Beyond the obvious, this one is load-bearing for
// LARGE_INTEGER below: that union overlays `struct { DWORD LowPart; LONG HighPart; }`
// on a 64-bit LONGLONG, which only works if both halves are 4 bytes.
typedef int LONG;
typedef LONG *PLONG, *LPLONG;   // LONG, not `long`
typedef __int64 LONGLONG;
typedef __uint64 ULONGLONG;
typedef __int64 LONG64, *PLONG64;

#define CONST const
typedef wchar_t WCHAR;
typedef WCHAR *PWCHAR;
typedef WCHAR *LPWCH, *PWCH;
typedef CONST WCHAR *LPCWCH, *PCWCH;
typedef WCHAR *NWPSTR;
typedef WCHAR *LPWSTR, *PWSTR;
typedef CONST WCHAR *LPCWSTR, *PCWSTR;

typedef CHAR *PCHAR;
typedef CHAR *LPCH, *PCH;
typedef CONST CHAR *LPCCH, *PCCH;
typedef CHAR *NPSTR;
typedef CHAR *LPSTR, *PSTR;
typedef CONST CHAR *LPCSTR, *PCSTR;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef int    HRESULT;   // 32-bit on Windows, like LONG
typedef void  *HANDLE;

#define DECLARE_HANDLE(name) typedef HANDLE name
DECLARE_HANDLE(HINSTANCE);
typedef HINSTANCE HMODULE;

typedef struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    DWORD Offset;
    DWORD OffsetHigh;
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef LPVOID PSECURITY_ATTRIBUTES;
typedef LPVOID LPSECURITY_ATTRIBUTES;

#define __forceinline inline
#define __in_ecount(a)
#define __in_bcount(a)
