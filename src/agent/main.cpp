// instant-alt-tab-agent.exe: runs in the background (started at sign-in) and
// keeps the hooks active in whichever Explorer is the shell. Once Explorer has
// started, it works out what the hooks need from its Alt+Tab DLLs
// (src/discovery), loads the one Explorer loads only on the first Alt+Tab,
// starts the hooks, and starts them again after Explorer restarts. Windowless;
// logs to agent.log next to itself.

#include "ComponentRegistration.h"
#include "CrashGuard.h"
#include "ExplorerProcess.h"
#include "Alert.h"
#include "../discovery/Discovery.h"
#include "../discovery/PeFile.h"
#include "../shared/Log.h"
#include "../shared/Ipc.h"
#include "../shared/TextFile.h"

#include <windows.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr DWORD kPollMs = 2000;
constexpr DWORD kSettleMs = 1500;              // lets the Alt+Tab opening that loaded the switcher finish first
constexpr DWORD kFirstRetryMs = 60 * 1000;     // a failed symbol download is tried again after a minute,
constexpr DWORD kMaxRetryMs = 60 * 60 * 1000;  // then less often, up to hourly

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

const wchar_t* Describe(std::optional<DWORD> result) {
    if (!result) return L"Explorer didn't answer in time; leaving this Explorer alone";
    switch (*result) {
        case Ipc::kStarted: return L"started";
        case Ipc::kAlreadyRunning: return L"another copy is already active";
        case Ipc::kUnsupportedBuild: return L"Explorer's code isn't what discovery analysed (see hook.log)";
        case Ipc::kStartFailed: return L"starting the hooks failed (see hook.log)";
        case Ipc::kNeedsReload: return L"an earlier copy is still loaded; waiting for Explorer to restart";
        default: return L"the DLL couldn't be loaded";
    }
}

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory = path;
    return directory.substr(0, directory.rfind(L'\\') + 1);
}

// What the user was already told, one line each ("paused <buildKey>"), so a
// message isn't repeated at every sign-in.
constexpr wchar_t kNotifiedFile[] = L"notified.txt";
constexpr size_t kMaxNotified = 20;
constexpr DWORD kCancelWaitMs = 45000;  // a cancelled download stops within one 30 s read

constexpr wchar_t kPausedMessage[] =
    L"Alt+Tab is back to normal speed: this version of Windows changed Alt+Tab in a way Instant Alt+Tab doesn't "
    L"recognize, so it left Alt+Tab alone.\n\nRun \"instant-alt-tab status\" for details. You'll see this once per "
    L"Windows update.";
constexpr wchar_t kTurnedOffMessage[] =
    L"Instant Alt+Tab turned itself off: something in Explorer didn't behave as expected. Alt+Tab is back to normal "
    L"speed until Explorer restarts.\n\nDetails are in hook.log.";

// The buildKey of the two DLLs Explorer loaded, or of the switcher's file while
// it isn't loaded yet.
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

void ShowAlertOnce(const std::wstring& directory, const std::wstring& key, const wchar_t* message) {
    std::wstring path = directory + kNotifiedFile;
    std::vector<std::wstring> told;
    std::wstringstream lines(ReadTextFile(path));
    for (std::wstring line; std::getline(lines, line);) told.push_back(line);
    if (std::find(told.begin(), told.end(), key) != told.end()) return;
    told.push_back(key);
    if (told.size() > kMaxNotified) told.erase(told.begin(), told.end() - kMaxNotified);
    std::wstring content;
    for (const std::wstring& line : told) content += line + L"\n";
    WriteTextFile(path, content);
    ShowAlert(message);
}

// Runs discovery on a worker thread, so a stop request or Explorer exiting is
// noticed at once, even during the symbol download. Returns nothing, with
// `wake` set, when interrupted.
std::optional<Discovery::Result> DiscoverInBackground(const std::wstring& twinui, const std::wstring& switcher,
                                                      const std::wstring& cache, HANDLE stop, HANDLE explorer,
                                                      WakeReason& wake) {
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto result = std::make_shared<Discovery::Result>();
    std::thread worker([=] { *result = Discovery::Discover(twinui, switcher, cache, cancel); });
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
std::optional<AltTab::Internals> Discover(ExplorerProcess& explorer, const std::wstring& switcher, HANDLE stop,
                                          const std::wstring& directory, WakeReason& wake) {
    std::wstring twinui = explorer.ModulePath(AltTab::kTwinuiDll);
    auto twinuiImage = explorer.LoadedImage(AltTab::kTwinuiDll);
    if (twinui.empty() || !twinuiImage) {
        Log(L"Explorer %lu: its Alt+Tab DLLs couldn't be read; it's probably exiting", explorer.Pid());
        return std::nullopt;
    }
    for (DWORD retryMs = kFirstRetryMs;; retryMs = std::min(retryMs * 2, kMaxRetryMs)) {
        ULONGLONG started = GetTickCount64();
        std::optional<Discovery::Result> result =
            DiscoverInBackground(twinui, switcher, directory + L"symbols\\", stop, explorer.Handle(), wake);
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
            return result->internals;
        }
        if (!result->retryLater) {
            Log(L"Explorer %lu: Alt+Tab stays stock: %s", explorer.Pid(), result->error.c_str());
            WriteTextFile(directory + Ipc::kProblemFile, result->error);
            ShowAlertOnce(directory, L"paused " + BuildKey(explorer, switcher), kPausedMessage);
            return std::nullopt;
        }
        WriteTextFile(directory + Ipc::kProblemFile, L"waiting to download Windows' public symbols: " + result->error);
        Log(L"Explorer %lu: %s; trying again in %lu minutes", explorer.Pid(), result->error.c_str(), retryMs / 60000);
        wake = WaitFor(stop, explorer.Handle(), retryMs);
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
        std::optional<bool> loaded = explorer.Load(path);
        if (!loaded || !*loaded) {
            Log(L"Explorer %lu: loading %s failed%s", explorer.Pid(), path.c_str(),
                loaded ? L"" : L" (no answer in time)");
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

// Serves one Explorer process until it exits or the agent is asked to stop.
// Returns false when the agent should exit.
bool ServeExplorer(ExplorerProcess& explorer, HANDLE stop, const std::wstring& directory, CrashGuard& guard) {
    std::wstring dll = directory + Ipc::kHookDll;
    WakeReason wake = WakeReason::Timeout;
    // Explorer loads twinui while it starts, and the switcher only on the first
    // Alt+Tab. The agent loads the switcher itself, from where Windows registered
    // it, so the first opening already buildKey the kept menu.
    if (!WaitForModule(explorer, AltTab::kTwinuiDll, stop, wake)) return wake == WakeReason::ExplorerExited;
    std::wstring switcher = explorer.ModulePath(AltTab::kSwitcherDll);
    if (switcher.empty()) switcher = RegisteredComponentPath(AltTab::kSwitcherDll);
    if (switcher.empty()) {
        Log(L"Explorer %lu: %s isn't registered; waiting for the first Alt+Tab to load it", explorer.Pid(),
            AltTab::kSwitcherDll);
        if (!WaitForModule(explorer, AltTab::kSwitcherDll, stop, wake)) return wake == WakeReason::ExplorerExited;
        switcher = explorer.ModulePath(AltTab::kSwitcherDll);
    }
    if (explorer.HasModule(AltTab::kSwitcherDll)) {  // loaded by an opening that may still be in progress
        wake = WaitFor(stop, explorer.Handle(), kSettleMs);
        if (wake != WakeReason::Timeout) return wake == WakeReason::ExplorerExited;
    }

    bool hooked;  // our copy is active in this Explorer
    if (explorer.IsHooked()) {
        hooked = explorer.HasLoaded(dll);  // left running by an agent that was killed
        Log(L"Explorer %lu: %s", explorer.Pid(), hooked ? L"hooks already active" : L"another copy is already active");
    } else {
        std::optional<AltTab::Internals> found = Discover(explorer, switcher, stop, directory, wake);
        if (wake != WakeReason::Timeout) return wake == WakeReason::ExplorerExited;
        if (found && !LoadSwitcher(explorer, switcher, found->switcher.module)) found.reset();
        std::optional<DWORD> result = found ? explorer.LoadAndStart(dll, *found) : std::nullopt;
        hooked = result == Ipc::kStarted;
        if (hooked) DeleteFileW((directory + Ipc::kProblemFile).c_str());
        if (found) Log(L"Explorer %lu: %s", explorer.Pid(), Describe(result));
    }

    HANDLE turnedOff =
        hooked ? OpenEventW(SYNCHRONIZE, FALSE, Ipc::TurnedOffEventName(explorer.Pid()).c_str()) : nullptr;
    while ((wake = WaitFor(stop, explorer.Handle(), INFINITE, turnedOff)) == WakeReason::TurnedOff) {
        Log(L"Explorer %lu: the hooks turned themselves off (see hook.log)", explorer.Pid());
        ShowAlertOnce(directory, L"turned off " + BuildKey(explorer, switcher), kTurnedOffMessage);
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
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, Ipc::kAgentStopEvent);
    HANDLE instance = CreateMutexW(nullptr, TRUE, Ipc::kAgentMutex);
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
