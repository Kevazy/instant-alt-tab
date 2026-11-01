#pragma once

// What the command-line tool, the agent and the hook DLL agree on.

#include <windows.h>

#include <string>

namespace Ipc {

inline constexpr wchar_t kAgentExecutable[] = L"instant-alt-tab-agent.exe";
inline constexpr wchar_t kHookDll[] = L"instant-alt-tab-hook.dll";

// The running agent: it owns the mutex until it exits and exits when the event is set.
inline constexpr wchar_t kAgentMutex[] = L"Local\\InstantAltTab-Agent";
inline constexpr wchar_t kAgentStopEvent[] = L"Local\\InstantAltTab-AgentStop";

// Written by the agent next to itself when Alt+Tab stays stock: the reason, for `status`.
inline constexpr wchar_t kProblemFile[] = L"problem.txt";

// Exports of the hook DLL, called in Explorer on a remote thread: DWORD WINAPI f(void*).
inline constexpr char kStartExport[] = "Start";
inline constexpr char kStopExport[] = "Stop";

enum StartResult : DWORD {
    kStarted = 1,
    kAlreadyRunning = 2,
    kUnsupportedBuild = 3,  // this Windows build isn't the one the hooks were made for
    kStartFailed = 4,
    kNeedsReload = 5,  // this copy was stopped and is still loaded; Explorer has to restart
};

enum StopResult : DWORD {
    kStopped = 1,  // hooks removed; the DLL can be unloaded
    kNotRunning = 2,
    kStoppedPinned = 3,  // hooks removed, but code may still run: the DLL pinned itself and stays loaded
};

// Exists while a copy of the hook DLL is started in the Explorer process with this id.
inline std::wstring HookedMarkerName(DWORD explorerPid) {
    return L"Local\\InstantAltTab-Hooked-" + std::to_wstring(explorerPid);
}

// Exists while that copy keeps the menu; it goes away when the hooks turn themselves off.
inline std::wstring ActiveMarkerName(DWORD explorerPid) {
    return L"Local\\InstantAltTab-Active-" + std::to_wstring(explorerPid);
}

// Set when the hooks turn themselves off because something unexpected happened
// (not on a stop), so the agent can tell the user.
inline std::wstring TurnedOffEventName(DWORD explorerPid) {
    return L"Local\\InstantAltTab-TurnedOff-" + std::to_wstring(explorerPid);
}

}  // namespace Ipc
