// instant-alt-tab-agent.exe: runs in the background (started at sign-in) and
// keeps the hooks active in whichever Explorer is the shell. It waits for
// Explorer's Alt+Tab code to load, works out what the hooks need from those
// DLLs (src/discovery), starts the hooks, and starts them again after Explorer
// restarts. Windowless; logs to agent.log next to itself.

#include "crash_guard.h"
#include "explorer.h"
#include "notification.h"
#include "../discovery/discovery.h"
#include "../shared/log.h"
#include "../shared/protocol.h"
#include "../shared/text_file.h"

#include <windows.h>

#include <algorithm>
#include <string>

namespace {

constexpr DWORD kPollMs = 2000;
constexpr DWORD kSettleMs = 1500;              // lets the Alt+Tab opening that loaded the modules finish first
constexpr DWORD kFirstRetryMs = 60 * 1000;     // a failed symbol download is tried again after a minute,
constexpr DWORD kMaxRetryMs = 60 * 60 * 1000;  // then less often, up to hourly

enum class Wake { Timeout, Stop, ExplorerExited, TurnedOff };

Wake WaitFor(HANDLE stop, HANDLE explorer, DWORD timeoutMs, HANDLE turnedOff = nullptr) {
    HANDLE handles[] = {stop, explorer, turnedOff};
    DWORD count = explorer ? (turnedOff ? 3 : 2) : 1;
    switch (WaitForMultipleObjects(count, handles, FALSE, timeoutMs)) {
        case WAIT_OBJECT_0: return Wake::Stop;
        case WAIT_OBJECT_0 + 1: return Wake::ExplorerExited;
        case WAIT_OBJECT_0 + 2: return Wake::TurnedOff;
        case WAIT_TIMEOUT: return Wake::Timeout;
        default: return Wake::Stop;  // a wait that fails can't be relied on again
    }
}

const wchar_t* Describe(std::optional<DWORD> result) {
    if (!result) return L"Explorer didn't answer in time; leaving this Explorer alone";
    switch (*result) {
        case protocol::kStarted: return L"started";
        case protocol::kAlreadyRunning: return L"another copy is already active";
        case protocol::kUnsupportedBuild: return L"Explorer's code isn't what discovery analysed (see hook.log)";
        case protocol::kStartFailed: return L"starting the hooks failed (see hook.log)";
        case protocol::kNeedsReload: return L"an earlier copy is still loaded; waiting for Explorer to restart";
        default: return L"the DLL couldn't be loaded";
    }
}

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory = path;
    return directory.substr(0, directory.rfind(L'\\') + 1);
}

// Why Alt+Tab stays stock, kept for `status` (protocol::kProblemFile). The
// user is told once per pair of DLL builds, not at every sign-in.
void ReportProblem(const std::wstring& directory, const std::wstring& builds, const std::wstring& reason) {
    WriteTextFile(directory + protocol::kProblemFile, reason);
    std::wstring notified = directory + L"notified.txt";
    if (ReadTextFile(notified) == builds) return;
    WriteTextFile(notified, builds);
    Notify(L"Alt+Tab is back to normal speed: this version of Windows changed Alt+Tab in a way Instant Alt+Tab "
           L"doesn't recognize, so it left Alt+Tab alone.\n\nRun \"instant-alt-tab status\" for details. You'll see "
           L"this once per Windows update.");
}

// Works out what the hooks need from the DLLs this Explorer loaded. Retries
// while the symbol download fails; returns nothing when the hooks must stay
// off for this Explorer, or when `wake` says the wait was interrupted.
std::optional<internals::Internals> Discover(Explorer& explorer, HANDLE stop, const std::wstring& directory,
                                             Wake& wake) {
    std::wstring twinui = explorer.ModulePath(internals::kTwinuiDll);
    std::wstring switcher = explorer.ModulePath(internals::kSwitcherDll);
    auto twinuiImage = explorer.LoadedImage(internals::kTwinuiDll);
    auto switcherImage = explorer.LoadedImage(internals::kSwitcherDll);
    wchar_t builds[64];
    swprintf_s(builds, L"%08x-%08x", twinuiImage ? twinuiImage->timeDateStamp : 0,
               switcherImage ? switcherImage->timeDateStamp : 0);
    for (DWORD retryMs = kFirstRetryMs;; retryMs = std::min(retryMs * 2, kMaxRetryMs)) {
        ULONGLONG started = GetTickCount64();
        discovery::Result result = discovery::Discover(twinui, switcher, directory + L"symbols\\");
        if (result.internals && (!twinuiImage || !switcherImage || result.internals->twinui.module != *twinuiImage ||
                                 result.internals->switcher.module != *switcherImage)) {
            // Rare: the files were replaced while Explorer kept the old ones loaded.
            Log(L"Explorer %lu: the Alt+Tab DLLs on disk aren't the ones it loaded; waiting for it to restart",
                explorer.Pid());
            return std::nullopt;
        }
        if (result.internals) {
            Log(L"Explorer %lu: found the Alt+Tab internals in %llu ms", explorer.Pid(), GetTickCount64() - started);
            return result.internals;
        }
        if (!result.retryLater) {
            Log(L"Explorer %lu: Alt+Tab stays stock: %s", explorer.Pid(), result.error.c_str());
            ReportProblem(directory, builds, result.error);
            return std::nullopt;
        }
        Log(L"Explorer %lu: %s; trying again in %lu minutes", explorer.Pid(), result.error.c_str(), retryMs / 60000);
        wake = WaitFor(stop, explorer.Handle(), retryMs);
        if (wake != Wake::Timeout) return std::nullopt;
    }
}

// Serves one Explorer process until it exits or the agent is asked to stop.
// Returns false when the agent should exit.
bool ServeExplorer(Explorer& explorer, HANDLE stop, const std::wstring& directory, CrashGuard& guard) {
    std::wstring dll = directory + protocol::kHookDll;
    // Explorer loads its Alt+Tab code only when Alt+Tab is first used.
    while (!explorer.HasAltTabModules()) {
        Wake wake = WaitFor(stop, explorer.Handle(), kPollMs);
        if (wake != Wake::Timeout) return wake == Wake::ExplorerExited;
    }
    if (Wake wake = WaitFor(stop, explorer.Handle(), kSettleMs); wake != Wake::Timeout) {
        return wake == Wake::ExplorerExited;
    }

    bool hooked;  // our copy is active in this Explorer
    if (explorer.IsHooked()) {
        hooked = explorer.HasLoaded(dll);  // left running by an agent that was killed
        Log(L"Explorer %lu: %s", explorer.Pid(), hooked ? L"hooks already active" : L"another copy is already active");
    } else {
        Wake wake = Wake::Timeout;
        std::optional<internals::Internals> found = Discover(explorer, stop, directory, wake);
        if (wake != Wake::Timeout) return wake == Wake::ExplorerExited;
        std::optional<DWORD> result = found ? explorer.LoadAndStart(dll, *found) : std::nullopt;
        hooked = result == protocol::kStarted;
        if (hooked) DeleteFileW((directory + protocol::kProblemFile).c_str());
        if (found) Log(L"Explorer %lu: %s", explorer.Pid(), Describe(result));
    }

    HANDLE turnedOff =
        hooked ? OpenEventW(SYNCHRONIZE, FALSE, protocol::TurnedOffEventName(explorer.Pid()).c_str()) : nullptr;
    Wake wake;
    while ((wake = WaitFor(stop, explorer.Handle(), INFINITE, turnedOff)) == Wake::TurnedOff) {
        Log(L"Explorer %lu: the hooks turned themselves off (see hook.log)", explorer.Pid());
        Notify(L"Instant Alt+Tab turned itself off: something in Explorer didn't behave as expected. Alt+Tab is "
               L"back to normal speed until Explorer restarts.\n\nDetails are in hook.log.");
        CloseHandle(turnedOff);
        turnedOff = nullptr;
    }
    if (turnedOff) CloseHandle(turnedOff);
    if (wake == Wake::Stop) {
        if (hooked) {
            std::optional<DWORD> result = explorer.StopAndUnload(dll);
            Log(L"Explorer %lu: hooks removed (result %ld)", explorer.Pid(), result ? static_cast<long>(*result) : -1L);
        }
        return false;
    }
    DWORD exitCode = explorer.ExitCode();
    Log(L"Explorer %lu exited (0x%08lx)", explorer.Pid(), exitCode);
    if (hooked && CrashGuard::IsCrash(exitCode) && !GetSystemMetrics(SM_SHUTTINGDOWN) && guard.RecordCrash()) {
        Log(L"Explorer crashed twice within an hour with the hooks active; disabled until the next install");
        return false;
    }
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::wstring directory = ExecutableDirectory();
    SetLogFile(directory + L"agent.log");
    // The stop event exists before the mutex, so a stop request can't be missed.
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, protocol::kAgentStopEvent);
    HANDLE instance = CreateMutexW(nullptr, TRUE, protocol::kAgentMutex);
    if (!stop || !instance || GetLastError() == ERROR_ALREADY_EXISTS) return 0;  // another agent is running

    CrashGuard guard(directory);
    if (guard.Disabled()) {
        Log(L"disabled by the crash guard; install again to re-enable");
    } else {
        Log(L"agent started");
        for (;;) {
            Explorer explorer = Explorer::Find();
            if (explorer.IsValid() && !ServeExplorer(explorer, stop, directory, guard)) break;
            if (WaitFor(stop, nullptr, kPollMs) == Wake::Stop) break;  // no shell yet, or a restarted one settling
        }
        Log(L"agent stopped");
    }
    // The set stop event goes first: an agent started right after this one must not find it.
    CloseHandle(stop);
    ReleaseMutex(instance);
    CloseHandle(instance);
    return 0;
}
