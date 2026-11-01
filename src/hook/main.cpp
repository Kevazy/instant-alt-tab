// The hook DLL. The agent loads it into Explorer and calls Start on a remote
// thread, passing what discovery found; Stop undoes everything so the agent
// can unload it again.

#include "card_list.h"
#include "hooking.h"
#include "persistent_menu.h"
#include "../shared/log.h"
#include "../shared/protocol.h"

#include <windows.h>

#include <string>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

constexpr DWORD kUiThreadTimeoutMs = 10000;
constexpr DWORD kDrainTimeoutMs = 5000;

SRWLOCK g_lock = SRWLOCK_INIT;  // Start and Stop never overlap
bool g_running = false;
bool g_shutDown = false;  // the hooks' state isn't reset, so this copy has to be reloaded to start again
DWORD g_stopResult = protocol::kNotRunning;
HANDLE g_hookedMarker = nullptr;

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    std::wstring directory = path;
    return directory.substr(0, directory.rfind(L'\\') + 1);
}

// Keeps this DLL loaded until Explorer exits: a hooked call or the UI thread
// may still run its code, so FreeLibrary from the agent must become a no-op.
void PinModule() {
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&__ImageBase), &self);
}

// Turns everything off; returns false if code of this DLL may still run.
bool Shutdown() {
    g_shutDown = true;
    bool uiThreadDone = persistent_menu::Stop(kUiThreadTimeoutMs);
    DisableHooks();
    return ReleaseHooks(uiThreadDone ? kDrainTimeoutMs : 0) && uiThreadDone;
}

DWORD StartHooks(const internals::Internals& found) {
    auto twinui = FindModule(internals::kTwinuiDll, found.twinui.module);
    auto switcher = FindModule(internals::kSwitcherDll, found.switcher.module);
    if (!twinui || !switcher) {
        Log(L"not started: the loaded %s isn't the file the agent analysed",
            !twinui ? internals::kTwinuiDll : internals::kSwitcherDll);
        return protocol::kUnsupportedBuild;
    }
    if (!InitializeHooks()) return protocol::kStartFailed;
    if (!card_list::Install(*switcher, found.switcher) || !persistent_menu::Install(*twinui, *switcher, found)) {
        ReleaseHooks(0);  // nothing was enabled
        Log(L"not started: a function's code in memory isn't what the agent analysed, or creating a hook failed");
        return protocol::kUnsupportedBuild;
    }
    persistent_menu::Enable();
    if (!EnableHooks()) {
        Log(L"not started: enabling the hooks failed");
        // Some hooks may have been enabled, and one may already have run.
        if (!Shutdown()) PinModule();
        return protocol::kStartFailed;
    }
    return protocol::kStarted;
}

}  // namespace

// `parameter` points to the agent's internals::Internals, copied into Explorer.
extern "C" __declspec(dllexport) DWORD WINAPI Start(void* parameter) {
    AcquireSRWLockExclusive(&g_lock);
    DWORD result;
    auto found = static_cast<const internals::Internals*>(parameter);
    if (!found || found->magic != internals::kMagic || found->version != internals::kVersion ||
        found->size != sizeof(internals::Internals)) {
        result = protocol::kStartFailed;  // an agent from another release
    } else if (g_running) {
        result = protocol::kAlreadyRunning;
    } else if (g_shutDown) {
        result = protocol::kNeedsReload;
    } else {
        SetLogFile(ModuleDirectory() + L"hook.log");
        // Another copy (another path, another version) may already hook this Explorer.
        g_hookedMarker = CreateEventW(nullptr, TRUE, TRUE, protocol::HookedMarkerName(GetCurrentProcessId()).c_str());
        if (!g_hookedMarker || GetLastError() == ERROR_ALREADY_EXISTS) {
            result = protocol::kAlreadyRunning;
        } else {
            result = StartHooks(*found);
        }
        if (result == protocol::kStarted) {
            g_running = true;
            Log(L"started in Explorer %lu", GetCurrentProcessId());
        } else if (g_hookedMarker) {
            CloseHandle(g_hookedMarker);
            g_hookedMarker = nullptr;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

extern "C" __declspec(dllexport) DWORD WINAPI Stop(void*) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_running) {
        // The kept menu belongs to Explorer's UI thread; it is dropped there.
        bool released = Shutdown();
        g_running = false;
        CloseHandle(g_hookedMarker);
        g_hookedMarker = nullptr;
        if (released) {
            g_stopResult = protocol::kStopped;
            Log(L"stopped");
        } else {
            PinModule();
            g_stopResult = protocol::kStoppedPinned;
            Log(L"stopped; code of this DLL may still run, so it stays loaded until Explorer restarts");
        }
    }
    // A second Stop (from another caller, or after a timeout) reports the
    // outcome of the first, so a pinned copy is never treated as unloadable.
    DWORD result = g_stopResult;
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}
