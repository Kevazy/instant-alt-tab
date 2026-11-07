// instant-alt-tab-agent.exe: runs in the background (started at sign-in) and
// keeps the hooks active in whichever Explorer is the shell. Once Explorer has
// started, it works out what the hooks need from its Alt+Tab DLLs
// (src/discovery), loads the one Explorer loads only on the first Alt+Tab,
// starts the hooks, and starts them again after Explorer restarts. Windowless;
// logs to agent.log next to itself.

#include "Alert.h"
#include "CrashGuard.h"
#include "ExplorerProcess.h"
#include "RegisteredDllPath.h"
#include "../discovery/Discovery.h"
#include "../discovery/PeFile.h"
#include "../shared/HookExports.h"
#include "../shared/Log.h"
#include "../shared/SharedNames.h"
#include "../shared/TextFile.h"

#include <windows.h>

#include <algorithm>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr DWORD kPollMs = 2000;                     // how often to look for the shell Explorer and its Alt+Tab modules
constexpr DWORD kSettleMs = 1500;                   // lets the Alt+Tab opening that loaded the switcher finish first
constexpr DWORD kDownloadRetryFirstMs = 60 * 1000;  // a failed symbol download is tried again after a minute,
constexpr DWORD kDownloadRetryMaxMs = 60 * 60 * 1000;       // then less often, up to hourly
constexpr DWORD kLongWaitStepMs = 60 * 60 * 1000;           // long waits are made of pieces this long, so sleep counts
constexpr int kFailedDownloadsBeforeNotice = 5;             // failures at 0, 1, 3, 7, 15 min: not just a slow sign-in
constexpr DWORD kUnpublishedRetryMs = 24 * 60 * 60 * 1000;  // symbols Microsoft hasn't published: daily

enum class WakeReason { Timeout, Stop, ExplorerExited, TurnedOff };

WakeReason WaitFor(HANDLE stop, HANDLE explorer, DWORD timeoutMs, HANDLE turnedOff = nullptr) {
    HANDLE handles[] = {stop, explorer, turnedOff};
    DWORD count = explorer ? (turnedOff ? 3 : 2) : 1;
    switch (WaitForMultipleObjects(count, handles, FALSE, timeoutMs)) {
        case WAIT_OBJECT_0: return WakeReason::Stop;
        case WAIT_OBJECT_0 + 1: return WakeReason::ExplorerExited;
        case WAIT_OBJECT_0 + 2: return WakeReason::TurnedOff;
        case WAIT_TIMEOUT: return WakeReason::Timeout;
        default: return WakeReason::Stop;  // a wait that fails can't be relied on again
    }
}

// Waits `ms` including time the PC sleeps, which a wait's timeout doesn't count:
// a long wait is made of pieces of at most an hour, measured with the tick
// count, which does count sleep and isn't moved by clock changes.
WakeReason WaitCountingSleep(HANDLE stop, HANDLE explorer, ULONGLONG ms) {
    for (ULONGLONG until = GetTickCount64() + ms, current = GetTickCount64(); current < until;
         current = GetTickCount64()) {
        WakeReason wake =
            WaitFor(stop, explorer, static_cast<DWORD>(std::min<ULONGLONG>(until - current, kLongWaitStepMs)));
        if (wake != WakeReason::Timeout) return wake;
    }
    return WakeReason::Timeout;
}

const wchar_t* Describe(std::optional<DWORD> result) {
    if (!result) return L"Explorer didn't answer in time; leaving this Explorer alone";
    switch (*result) {
        case HookExports::kStarted: return L"started";
        case HookExports::kAlreadyRunning: return L"another copy is already active";
        case HookExports::kUnsupportedBuild: return L"Explorer's code isn't what discovery analysed (see hook.log)";
        case HookExports::kStartFailed: return L"starting the hooks failed (see hook.log)";
        case HookExports::kNeedsReload: return L"an earlier copy is still loaded; waiting for Explorer to restart";
        default: return L"the DLL couldn't be loaded";
    }
}

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory = path;
    return directory.substr(0, directory.rfind(L'\\') + 1);
}

constexpr DWORD kCancelWaitMs = 45000;  // a cancelled download stops within one 30 s read

constexpr wchar_t kPausedMessage[] =
    L"Alt+Tab is back to normal speed: this version of Windows changed Alt+Tab in a way Instant Alt+Tab doesn't "
    L"recognize, so it left Alt+Tab alone.\n\nRun \"instant-alt-tab status\" for details. You'll see this once per "
    L"Windows update.";
constexpr wchar_t kUnpublishedMessage[] =
    L"Alt+Tab is at normal speed for now: Microsoft hasn't yet published the debugging symbols Instant Alt+Tab "
    L"needs for this version of Windows. It checks again once a day and speeds Alt+Tab up as soon as they're "
    L"available.";
constexpr wchar_t kOfflineMessage[] =
    L"Alt+Tab is at normal speed for now: after a Windows update, Instant Alt+Tab downloads a file from "
    L"Microsoft's symbol server once, and that hasn't worked yet (see \"instant-alt-tab status\"). It keeps "
    L"trying, and speeds Alt+Tab up as soon as it succeeds.";
constexpr wchar_t kTurnedOffMessage[] =
    L"Instant Alt+Tab turned itself off: something in Explorer didn't behave as expected. Alt+Tab is back to normal "
    L"speed until Explorer restarts.\n\nDetails are in hook.log.";

// Names the builds of the two DLLs, from the ones Explorer loaded (or the
// switcher's file while it isn't loaded yet), so each message is shown once
// per Windows update.
std::wstring BuildKey(const ExplorerProcess& explorer, const std::wstring& switcherPath) {
    auto twinui = explorer.LoadedImage(AltTab::kTwinuiDll);
    uint32_t switcher = 0;
    if (auto image = explorer.LoadedImage(AltTab::kSwitcherDll)) {
        switcher = image->timeDateStamp;
    } else if (auto file = Discovery::PeFile::Load(switcherPath)) {
        switcher = file->TimeDateStamp();
    }
    wchar_t buildKey[32];
    swprintf_s(buildKey, L"%08x-%08x", twinui ? twinui->timeDateStamp : 0, switcher);
    return buildKey;
}

// Runs discovery on a worker thread, so a stop request or Explorer exiting is
// noticed at once, even during the symbol download. Returns nothing, with
// `wake` set, when interrupted.
std::optional<Discovery::Result> DiscoverInBackground(const Discovery::DllPaths& dlls, const std::wstring& cache,
                                                      HANDLE stop, HANDLE explorer, WakeReason& wake) {
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto result = std::make_shared<Discovery::Result>();
    std::thread worker([=] { *result = Discovery::Discover(dlls, cache, cancel); });
    HANDLE handles[] = {stop, explorer, worker.native_handle()};
    DWORD waited = WaitForMultipleObjects(3, handles, FALSE, INFINITE);
    if (waited == WAIT_OBJECT_0 + 2) {
        worker.join();
        CloseHandle(cancel);
        return *result;
    }
    wake = waited == WAIT_OBJECT_0 + 1 ? WakeReason::ExplorerExited : WakeReason::Stop;
    SetEvent(cancel);
    // When stopping, the process exits right after; otherwise let the download
    // end first so the next discovery doesn't write the same file.
    if (WaitForSingleObject(worker.native_handle(), wake == WakeReason::Stop ? 2000 : kCancelWaitMs) == WAIT_OBJECT_0) {
        worker.join();
        CloseHandle(cancel);
    } else {
        worker.detach();  // it still uses `cancel`, so that stays open
    }
    return std::nullopt;
}

// Works out what the hooks need from the DLLs this Explorer loaded, or will
// load: `switcher` may not be loaded yet. Retries while the symbol download
// fails; returns nothing when the hooks must stay off for this Explorer, or
// when `wake` says the wait was interrupted.
std::optional<AltTab::Internals> DiscoverWithRetries(ExplorerProcess& explorer, const std::wstring& switcher,
                                                     HANDLE stop, const std::wstring& directory, WakeReason& wake) {
    std::wstring twinui = explorer.ModulePath(AltTab::kTwinuiDll);
    auto twinuiImage = explorer.LoadedImage(AltTab::kTwinuiDll);
    if (twinui.empty() || !twinuiImage) {
        Log(L"Explorer %lu: its Alt+Tab DLLs couldn't be read; it's probably exiting", explorer.Pid());
        return std::nullopt;
    }
    // Optional: the class of the shell's items, from the DLL that implements them. A
    // file whose symbols couldn't be downloaded isn't tried again while the agent
    // runs, so a network without the symbol server doesn't delay every Explorer start.
    static std::set<std::wstring> dataModelsWithoutSymbols;
    std::wstring dataModel = explorer.ModulePath(AltTab::kDataModelDll);
    if (dataModel.empty()) dataModel = RegisteredDllPath(AltTab::kDataModelDll);
    if (dataModelsWithoutSymbols.contains(dataModel)) dataModel.clear();
    int failedDownloads = 0;
    for (DWORD retryMs = kDownloadRetryFirstMs;; retryMs = std::min(retryMs * 2, kDownloadRetryMaxMs)) {
        ULONGLONG started = GetTickCount64();
        std::optional<Discovery::Result> result = DiscoverInBackground(
            {twinui, switcher, dataModel}, directory + SharedNames::kSymbolsFolder, stop, explorer.Handle(), wake);
        if (!result) return std::nullopt;
        auto switcherImage = explorer.LoadedImage(AltTab::kSwitcherDll);
        if (result->internals && (result->internals->twinui.module != *twinuiImage ||
                                  (switcherImage && result->internals->switcher.module != *switcherImage))) {
            // Rare: the files were replaced while Explorer kept the old ones loaded.
            Log(L"Explorer %lu: the Alt+Tab DLLs on disk aren't the ones it loaded; waiting for it to restart",
                explorer.Pid());
            return std::nullopt;
        }
        if (result->internals) {
            Log(L"Explorer %lu: found the Alt+Tab internals in %llu ms", explorer.Pid(), GetTickCount64() - started);
            DeleteFileW((directory + SharedNames::kProblemFile).c_str());  // a failed download before this one
            if (!result->internals->dataModel.itemVtables[0]) {
                Log(L"Explorer %lu: %s's item class wasn't found (%s), so the second Alt+Tab after Explorer starts may "
                    L"rebuild the menu once",
                    explorer.Pid(), AltTab::kDataModelDll, result->dataModelError.c_str());
                if (result->dataModelDownloadFailed) dataModelsWithoutSymbols.insert(dataModel);
            }
            return result->internals;
        }
        if (!result->retryLater) {
            Log(L"Explorer %lu: Alt+Tab stays stock: %s", explorer.Pid(), result->error.c_str());
            WriteTextFile(directory + SharedNames::kProblemFile, result->error);
            Alert::ShowOnce(directory, L"paused " + BuildKey(explorer, switcher), kPausedMessage);
            return std::nullopt;
        }
        DWORD waitMs = retryMs;
        if (result->notPublished) {
            waitMs = kUnpublishedRetryMs;
            Alert::ShowOnce(directory, L"unpublished " + BuildKey(explorer, switcher), kUnpublishedMessage);
        } else if (++failedDownloads == kFailedDownloadsBeforeNotice) {
            Alert::ShowOnce(directory, L"offline " + BuildKey(explorer, switcher), kOfflineMessage);
        }
        WriteTextFile(directory + SharedNames::kProblemFile, SharedNames::kProblemRetryingDownload + result->error);
        Log(L"Explorer %lu: %s; trying again in %lu minutes", explorer.Pid(), result->error.c_str(), waitMs / 60000);
        wake = result->notPublished ? WaitCountingSleep(stop, explorer.Handle(), waitMs)
                                    : WaitFor(stop, explorer.Handle(), waitMs);
        if (wake != WakeReason::Timeout) return std::nullopt;
    }
}

// Waits until Explorer has loaded the module. Returns false, with `wake` set,
// when the wait was interrupted.
bool WaitForModule(const ExplorerProcess& explorer, const wchar_t* name, HANDLE stop, WakeReason& wake) {
    while (!explorer.HasModule(name)) {
        wake = WaitFor(stop, explorer.Handle(), kPollMs);
        if (wake != WakeReason::Timeout) return false;
    }
    return true;
}

// Loads the switcher DLL if Explorer hasn't yet, so the hooks are in place
// for the first Alt+Tab, which would load the same file. Returns false if it
// couldn't be loaded, or the loaded file isn't the one discovery analysed.
bool LoadSwitcher(ExplorerProcess& explorer, const std::wstring& path, const AltTab::ImageIdentity& analysed) {
    if (!explorer.HasModule(AltTab::kSwitcherDll)) {
        if (!explorer.Load(path).has_value()) {
            Log(L"Explorer %lu: loading %s didn't finish in time", explorer.Pid(), path.c_str());
            return false;
        }
        // Checked by name, not by the path Load used: Windows may spell the loaded path differently.
        if (!explorer.HasModule(AltTab::kSwitcherDll)) {
            Log(L"Explorer %lu: loading %s failed", explorer.Pid(), path.c_str());
            return false;
        }
        Log(L"Explorer %lu: loaded %s ahead of the first Alt+Tab", explorer.Pid(), AltTab::kSwitcherDll);
    }
    if (explorer.LoadedImage(AltTab::kSwitcherDll) != analysed) {
        Log(L"Explorer %lu: the loaded %s isn't the file discovery analysed", explorer.Pid(), AltTab::kSwitcherDll);
        return false;
    }
    return true;
}

// The switcher DLL this Explorer loaded or will load: the loaded file, else the
// file Windows registered it as. If it isn't registered, or that file can't be
// read, waits for the first Alt+Tab to load it. Returns nothing, with `wake`
// set, when a wait was interrupted.
std::wstring FindSwitcherPath(ExplorerProcess& explorer, HANDLE stop, WakeReason& wake) {
    std::wstring path = explorer.ModulePath(AltTab::kSwitcherDll);
    if (!path.empty()) return path;
    path = RegisteredDllPath(AltTab::kSwitcherDll);
    if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    Log(L"Explorer %lu: %s %s; waiting for the first Alt+Tab to load it", explorer.Pid(), AltTab::kSwitcherDll,
        path.empty() ? L"isn't registered" : L"is registered as a file that doesn't exist");
    if (!WaitForModule(explorer, AltTab::kSwitcherDll, stop, wake)) return L"";
    path = explorer.ModulePath(AltTab::kSwitcherDll);
    if (path.empty()) {  // loaded, but unreadable: Explorer is exiting
        Log(L"Explorer %lu: its Alt+Tab DLLs couldn't be read; it's probably exiting", explorer.Pid());
        wake = WaitFor(stop, explorer.Handle(), INFINITE);
    }
    return path;
}

// Works out what the hooks need, loads the switcher DLL if Explorer hasn't,
// and starts the hooks. `injected` is set once the agent loads anything into
// Explorer. Returns the Start result, or nothing if the hooks weren't started
// or `wake` says a wait was interrupted.
std::optional<DWORD> StartHooks(ExplorerProcess& explorer, const std::wstring& switcher, const std::wstring& dll,
                                HANDLE stop, const std::wstring& directory, WakeReason& wake, bool& injected) {
    bool explorerHadSwitcher = explorer.HasModule(AltTab::kSwitcherDll);
    std::optional<AltTab::Internals> found = DiscoverWithRetries(explorer, switcher, stop, directory, wake);
    if (!found) return std::nullopt;
    // An Alt+Tab during discovery loaded the switcher: let that opening finish first.
    if (!explorerHadSwitcher && explorer.HasModule(AltTab::kSwitcherDll)) {
        wake = WaitFor(stop, explorer.Handle(), kSettleMs);
        if (wake != WakeReason::Timeout) return std::nullopt;
    }
    injected = true;
    // Discovery records its own reason when it stops; from here on, this says why the hooks aren't on.
    std::wstring problemFile = directory + SharedNames::kProblemFile;
    if (!LoadSwitcher(explorer, switcher, found->switcher.module)) {
        WriteTextFile(problemFile, std::wstring(L"loading ") + AltTab::kSwitcherDll +
                                       L" into Explorer failed, or it loaded another file than the one analysed");
        return std::nullopt;
    }
    std::optional<DWORD> result = explorer.LoadAndStart(dll, *found);
    Log(L"Explorer %lu: %s", explorer.Pid(), Describe(result));
    if (result == HookExports::kStarted) {
        DeleteFileW(problemFile.c_str());
    } else {
        WriteTextFile(problemFile, Describe(result));
    }
    return result;
}

// Serves one Explorer process until it exits or the agent is asked to stop.
// Returns false when the agent should exit.
bool ServeExplorer(ExplorerProcess& explorer, HANDLE stop, const std::wstring& directory, CrashGuard& guard) {
    std::wstring dll = directory + SharedNames::kHookDll;
    WakeReason wake = WakeReason::Timeout;
    // Explorer loads twinui while it starts, and the switcher only on the first
    // Alt+Tab. The agent loads the switcher itself, from where Windows registered
    // it, so the first opening already builds the kept menu.
    if (!WaitForModule(explorer, AltTab::kTwinuiDll, stop, wake)) return wake == WakeReason::ExplorerExited;
    std::wstring switcher = FindSwitcherPath(explorer, stop, wake);
    if (switcher.empty()) return wake == WakeReason::ExplorerExited;
    if (explorer.HasModule(AltTab::kSwitcherDll)) {  // loaded by an opening that may still be in progress
        wake = WaitFor(stop, explorer.Handle(), kSettleMs);
        if (wake != WakeReason::Timeout) return wake == WakeReason::ExplorerExited;
    }

    bool hooked;            // our copy is active in this Explorer
    bool injected = false;  // the agent loaded something into it: a crash from then on counts
    if (explorer.IsHooked()) {
        hooked = injected = explorer.HasLoaded(dll);  // left running by an agent that was killed
        Log(L"Explorer %lu: %s", explorer.Pid(), hooked ? L"hooks already active" : L"another copy is already active");
    } else {
        DeleteFileW((directory + SharedNames::kProblemFile).c_str());  // about an earlier Explorer
        std::optional<DWORD> result = StartHooks(explorer, switcher, dll, stop, directory, wake, injected);
        if (wake != WakeReason::Timeout) return wake == WakeReason::ExplorerExited;
        hooked = result == HookExports::kStarted;
    }

    HANDLE turnedOff =
        hooked ? OpenEventW(SYNCHRONIZE, FALSE, SharedNames::TurnedOffEventName(explorer.Pid()).c_str()) : nullptr;
    while ((wake = WaitFor(stop, explorer.Handle(), INFINITE, turnedOff)) == WakeReason::TurnedOff) {
        Log(L"Explorer %lu: the hooks turned themselves off (see hook.log)", explorer.Pid());
        Alert::ShowOnce(directory, L"turned off " + BuildKey(explorer, switcher), kTurnedOffMessage);
        CloseHandle(turnedOff);
        turnedOff = nullptr;
    }
    if (turnedOff) CloseHandle(turnedOff);
    if (wake == WakeReason::Stop) {
        if (hooked) {
            std::optional<DWORD> result = explorer.StopAndUnload(dll);
            Log(L"Explorer %lu: hooks removed (result %ld)", explorer.Pid(), result ? static_cast<long>(*result) : -1L);
        }
        return false;
    }
    DWORD exitCode = explorer.ExitCode();
    Log(L"Explorer %lu exited (0x%08lx)", explorer.Pid(), exitCode);
    if (injected && CrashGuard::IsCrash(exitCode) && !GetSystemMetrics(SM_SHUTTINGDOWN) && guard.RecordCrash()) {
        Log(L"Explorer crashed twice within an hour with Instant Alt+Tab active in it; disabled until the next "
            L"install");
        return false;
    }
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::wstring directory = ExecutableDirectory();
    SetLogFile(directory + SharedNames::kAgentLog);
    // The stop event exists before the mutex, so a stop request can't be missed.
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, SharedNames::kAgentStopEvent);
    HANDLE instance = CreateMutexW(nullptr, TRUE, SharedNames::kAgentMutex);
    if (!stop || !instance || GetLastError() == ERROR_ALREADY_EXISTS) return 0;  // another agent is running

    CrashGuard guard(directory);
    if (guard.Disabled()) {
        Log(L"disabled by the crash guard; install again to re-enable");
    } else {
        Log(L"agent started");
        for (;;) {
            ExplorerProcess explorer = ExplorerProcess::Find();
            if (explorer.IsValid() && !ServeExplorer(explorer, stop, directory, guard)) break;
            if (WaitFor(stop, nullptr, kPollMs) == WakeReason::Stop)
                break;  // no shell yet, or a restarted one settling
        }
        Log(L"agent stopped");
    }
    // The set stop event goes first: an agent started right after this one must not find it.
    CloseHandle(stop);
    ReleaseMutex(instance);
    CloseHandle(instance);
    return 0;
}
