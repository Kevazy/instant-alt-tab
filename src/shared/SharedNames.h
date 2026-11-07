#pragma once

// Every name more than one of the three programs uses, or that exists outside
// the code: file names, the files the agent and the hook keep next to
// themselves, and named kernel objects. Installed copies and running agents
// rely on these, so they never change.

#include <windows.h>

#include <string>

namespace SharedNames {

inline constexpr wchar_t kAgentExecutable[] = L"instant-alt-tab-agent.exe";
inline constexpr wchar_t kHookDll[] = L"instant-alt-tab-hook.dll";

// Files in the agent's folder (the install folder, or the build folder when run from there).
inline constexpr wchar_t kAgentLog[] = L"agent.log";
inline constexpr wchar_t kHookLog[] = L"hook.log";
inline constexpr wchar_t kProblemFile[] = L"problem.txt";         // why Alt+Tab is stock (status, installer)
inline constexpr wchar_t kNotifiedFile[] = L"notified.txt";       // which messages the user has seen
inline constexpr wchar_t kCrashGuardFile[] = L"crash-guard.txt";  // recent Explorer crashes
inline constexpr wchar_t kSymbolsFolder[] = L"symbols\\";         // names looked up in Windows' public symbols
// Inside the symbols folder: holds a PDB only while one is downloaded and read.
inline constexpr wchar_t kSymbolsDownloadFolder[] = L"download\\";

// Next to a PDB being downloaded, with this added to its name: the PDB's full
// size in bytes, so the installer can show the download's progress.
inline constexpr wchar_t kDownloadSizeSuffix[] = L".size";

// How problem.txt starts while the agent keeps retrying the symbol download.
inline constexpr wchar_t kProblemRetryingDownload[] = L"waiting to download Windows' public symbols: ";

// The running agent: it owns the mutex until it exits and exits when the event is set.
inline constexpr wchar_t kAgentMutex[] = L"Local\\InstantAltTab-Agent";
inline constexpr wchar_t kAgentStopEvent[] = L"Local\\InstantAltTab-AgentStop";

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

// Set from the start of an opening until its dismissal, so also while a new menu is
// still being built. Only the installer uses it: Esc cancels an opening then, and
// without one, Esc with Alt down would be Alt+Esc.
inline std::wstring MenuOpeningEventName(DWORD explorerPid) {
    return L"Local\\InstantAltTab-MenuOpening-" + std::to_wstring(explorerPid);
}

// Set while the kept menu is on screen. Only the installer uses it, to know when the
// Alt+Tab it opens to build the menu can be closed again.
inline std::wstring MenuShownEventName(DWORD explorerPid) {
    return L"Local\\InstantAltTab-MenuShown-" + std::to_wstring(explorerPid);
}

// Set while the next Alt+Tab will show the kept menu instead of building one (as far as the
// last dismissal could tell: a window or display change can still make it rebuild).
inline std::wstring MenuReadyEventName(DWORD explorerPid) {
    return L"Local\\InstantAltTab-MenuReady-" + std::to_wstring(explorerPid);
}

}  // namespace SharedNames
