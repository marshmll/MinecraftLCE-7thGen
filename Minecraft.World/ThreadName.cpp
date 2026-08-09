#include "stdafx.h"

// From Xbox documentation

typedef struct tagTHREADNAME_INFO {
    DWORD dwType;     // Must be 0x1000
    LPCSTR szName;    // Pointer to name (in user address space)
    DWORD dwThreadID; // Thread ID (-1 for caller thread)
    DWORD dwFlags;    // Reserved for future use; must be zero
} THREADNAME_INFO;

void SetThreadName( DWORD dwThreadID, LPCSTR szThreadName )
{
#ifndef __PS3__
    THREADNAME_INFO info;
	
    info.dwType = 0x1000;
    info.szName = szThreadName;
    info.dwThreadID = dwThreadID;
    info.dwFlags = 0;
	
#if ( defined _WINDOWS64 | defined _DURANGO )
	__try
	{
		RaiseException( 0x406D1388, 0, sizeof(info)/sizeof(DWORD), (ULONG_PTR *)&info );
	}
	__except( GetExceptionCode()==0x406D1388 ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_EXECUTE_HANDLER )
	{
	}
#endif
#ifdef _XBOX
    __try
    {
        RaiseException( 0x406D1388, 0, sizeof(info)/sizeof(DWORD), (DWORD *)&info );
    }
    __except( GetExceptionCode()==0x406D1388 ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_EXECUTE_HANDLER )
    {
    }
#endif
#ifdef _LINUX64
	// Every caller passes -1/0 meaning "the calling thread", which is the only case
	// pthread_setname_np can serve anyway. Without this every thread in the process
	// shows up in top/btop/gdb as "Minecraft.Clien", which makes any question about
	// which thread is doing the work unanswerable - the names the engine already
	// supplies ("Rebuild Chunk Thread 0", "Server", "Chunk update", ...) are exactly
	// what is needed. The limit is 16 bytes including the NUL, so it has to be
	// truncated; the "(4J) " prefix C4JThread adds would eat a third of that, so drop
	// it here.
	if (szThreadName != NULL)
	{
		const char *shortName = szThreadName;
		if (strncmp(shortName, "(4J) ", 5) == 0)
			shortName += 5;

		char truncated[16];
		snprintf(truncated, sizeof(truncated), "%s", shortName);
		pthread_setname_np(pthread_self(), truncated);
	}
#endif
#endif // __PS3__
}
