#include "stdafx.h"
#include "../../PS3/PS3Extras/ShutdownManager.h"
#include "../../Common/Network/GameNetworkManager.h"
#include "../../MinecraftServer.h"
#include "LinuxAudioShim.h"

#include <atomic>

// ShutdownManager's declaration (Minecraft.Client/PS3/PS3Extras/
// ShutdownManager.h) is actually platform-generic despite its path: its
// public interface has no PS3-specific types, only its private section
// (guarded #ifdef __PS3__) does. GameRenderer.cpp/ServerLevel.cpp/
// MinecraftServer.cpp/Common/Network/Sony/SonyCommerce.cpp/C4JThread.cpp/
// Connection.cpp/Socket.cpp all unconditionally call into this on every
// platform - real per-thread stop-signaling, not incidental.
//
// A real, working reference implementation already exists in the leak at
// Minecraft.Client/PSVita/PSVitaExtras/ShutdownManager.cpp (also #ifdef
// __PS3__-gated inside a PSVita-path file, an odd but real pre-existing
// organization quirk). This ports that logic to Linux: without it, every
// background-thread loop this gates (GameRenderer's chunk-update thread,
// MinecraftServer's post-process/server threads, C4JThread's event-queue
// threads, Connection's read/write threads) runs forever, since our
// previous stub's ShouldRun() always returned true - so when the main
// thread returned from main() on window close, those threads were still
// live and racing the process's static-destructor teardown. That race was
// the crash on window close.
//
// The private static members/helpers (s_threadShouldRun, RequestThreadToStop,
// etc.) are declared #ifdef __PS3__ only in the shared header, so they
// aren't members of the class here - this uses its own file-local state
// instead, calling only the class's public interface.

namespace
{
    // Atomic, not plain int/bool: these are read in WaitForSignalledToComplete()'s
    // spin loop, whose only other statement is Sleep(). They have internal
    // linkage, so the compiler can prove Sleep() cannot modify them and is free
    // to hoist the loads out of the loop - which would spin forever no matter
    // what the other threads write. Relaxed ordering is enough; these are
    // standalone flags/counters, not a handoff of other data.
    std::atomic<bool> s_threadShouldRun[ShutdownManager::eThreadIdCount];
    std::atomic<int> s_threadRunning[ShutdownManager::eThreadIdCount];
    CRITICAL_SECTION s_threadRunningCS;
    C4JThread::EventArray *s_eventArray[ShutdownManager::eThreadIdCount];

    void RequestThreadToStop(int i)
    {
        s_threadShouldRun[i] = false;
        // EventArray::Cancel() is #ifdef __PS3__-only; SetAll() wakes any
        // thread blocked in WaitForAll/WaitForAny/WaitForSingle just the
        // same (they re-check ShouldRun() immediately after waking and exit
        // their loop), which is all RequestThreadToStop actually needs here.
        if (s_eventArray[i])
            s_eventArray[i]->SetAll();
    }

    void WaitForSignalledToComplete()
    {
        bool allComplete;
        do
        {
            Sleep(10);
            allComplete = true;
            for (int i = 0; i < ShutdownManager::eThreadIdCount; i++)
            {
                // "> 0", not "!= 0". A HasFinished() without a matching
                // HasStarted() drives the count negative, and "!= 0" then never
                // clears - the main thread spins here forever, the window is
                // never destroyed, and the game looks frozen on its last frame.
                // MinecraftServer::main() had exactly that asymmetry (its
                // HasStarted was #ifdef __PS3__-only while HasFinished was
                // unconditional); that is fixed at the source too, but the wait
                // should not be the thing that turns a miscount into a hang.
                if (!s_threadShouldRun[i] && s_threadRunning[i] > 0)
                    allComplete = false;
            }
        } while (!allComplete);
    }
}

void ShutdownManager::Initialise()
{
    for (int i = 0; i < eThreadIdCount; i++)
    {
        s_threadShouldRun[i] = true;
        s_threadRunning[i] = 0;
        s_eventArray[i] = NULL;
    }
    InitializeCriticalSection(&s_threadRunningCS);
}

void ShutdownManager::StartShutdown()
{
    s_threadShouldRun[eMainThread] = false;
}

void ShutdownManager::MainThreadHandleShutdown()
{
    app.DebugPrintf("Shutdown manager: waiting on first batch of threads requested to terminate...\n");
    RequestThreadToStop(eLeaderboardThread);
    RequestThreadToStop(eCommerceThread);
    RequestThreadToStop(ePostProcessThread);
    RequestThreadToStop(eRunUpdateThread);
    RequestThreadToStop(eRenderChunkUpdateThread);
    RequestThreadToStop(eConnectionReadThreads);
    RequestThreadToStop(eConnectionWriteThreads);
    RequestThreadToStop(eEventQueueThreads);
    WaitForSignalledToComplete();
    app.DebugPrintf("Shutdown manager: terminated.\n");

    app.DebugPrintf("Shutdown manager: waiting on server to terminate...\n");
    MinecraftServer::HaltServer();
    RequestThreadToStop(eServerThread);
    WaitForSignalledToComplete();
    app.DebugPrintf("Shutdown manager: terminated.\n");

    // No async ExitRequest/Terminate hook exists on LinuxStorage/ProfileManager
    // (that's a PSVita-only extension to C4JStorage/C_4JProfile, absent from
    // the Windows64/Linux 4J_Storage.h/4J_Profile.h headers this port reuses)
    // - LinuxStorage's operations are synchronous, so there's nothing
    // in-flight to wait for here.

    app.DebugPrintf("Shutdown manager: Audio shutdown.\n");
    AIL_shutdown();

    app.DebugPrintf("Shutdown manager: Network manager shutdown.\n");
    g_NetworkManager.Terminate();

    app.DebugPrintf("Shutdown manager: Complete.\n");
}

void ShutdownManager::HasStarted(EThreadId threadId)
{
    EnterCriticalSection(&s_threadRunningCS);
    s_threadRunning[threadId]++;
    LeaveCriticalSection(&s_threadRunningCS);
}

void ShutdownManager::HasStarted(EThreadId threadId, C4JThread::EventArray *eventArray)
{
    EnterCriticalSection(&s_threadRunningCS);
    s_threadRunning[threadId]++;
    LeaveCriticalSection(&s_threadRunningCS);
    s_eventArray[threadId] = eventArray;
}

bool ShutdownManager::ShouldRun(EThreadId threadId)
{
    return s_threadShouldRun[threadId];
}

void ShutdownManager::HasFinished(EThreadId threadId)
{
    EnterCriticalSection(&s_threadRunningCS);
    s_threadRunning[threadId]--;
    LeaveCriticalSection(&s_threadRunningCS);
}
