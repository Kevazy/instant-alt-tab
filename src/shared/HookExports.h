#pragma once

// The hook DLL's exports and what they return. The agent calls them in
// Explorer on a remote thread, so each has the thread-start signature
// DWORD WINAPI f(void* parameter) and returns one of the codes below.

#include <windows.h>

namespace HookExports {

// Start's parameter is the AltTab::Internals discovery found (AltTabInternals.h).
inline constexpr char kStartExport[] = "Start";
inline constexpr char kStopExport[] = "Stop";

enum StartResult : DWORD {
    kStarted = 1,
    kAlreadyRunning = 2,
    kUnsupportedBuild = 3,  // the loaded DLLs, or their code in memory, aren't what discovery analysed
    kStartFailed = 4,
    kNeedsReload = 5,  // this copy was stopped and is still loaded; Explorer has to restart
};

enum StopResult : DWORD {
    kStopped = 1,  // hooks removed; the DLL can be unloaded
    kNotRunning = 2,
    kStoppedPinned = 3,  // hooks removed, but code may still run: the DLL pinned itself and stays loaded
};

}  // namespace HookExports
