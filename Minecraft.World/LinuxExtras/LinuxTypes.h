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

typedef unsigned long       DWORD;
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
typedef long    *PLONG, *LPLONG;
typedef DWORD   *PDWORD, *LPDWORD;
typedef void    *PVOID, *LPVOID;
typedef const void *LPCVOID;
typedef unsigned int *PUINT;

typedef unsigned long ULONG;
typedef unsigned char boolean;
typedef unsigned long ULONG_PTR, *PULONG_PTR;
typedef ULONG_PTR SIZE_T, *PSIZE_T;

#define VOID void
typedef char CHAR;
typedef short SHORT;
typedef long LONG;
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

typedef long   HRESULT;
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
