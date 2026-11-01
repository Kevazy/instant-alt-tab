#include "explorer.h"

#include "../shared/protocol.h"

#include <psapi.h>

#include <utility>

namespace {

// Stop can take up to ~15 s by itself (it waits for Explorer's UI thread and for hooked calls).
constexpr DWORD kRemoteCallTimeoutMs = 30000;
constexpr wchar_t kTwinui[] = L"twinui.pcshell.dll";
constexpr wchar_t kSwitcher[] = L"WindowsInternal.ComposableShell.Experiences.Switcher.dll";

void* Kernel32Function(const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name));
}

// Offset of an export in the DLL, from a copy mapped here without running it.
DWORD ExportOffset(const std::wstring& dllPath, const char* name) {
    HMODULE local = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) return 0;
    auto address = reinterpret_cast<BYTE*>(GetProcAddress(local, name));
    DWORD offset = address ? static_cast<DWORD>(address - reinterpret_cast<BYTE*>(local)) : 0;
    FreeLibrary(local);
    return offset;
}

}  // namespace

Explorer Explorer::Find() {
    Explorer explorer;
    HWND shell = GetShellWindow();
    if (!shell || !GetWindowThreadProcessId(shell, &explorer.pid_)) return explorer;
    explorer.process_ = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                        PROCESS_VM_READ | PROCESS_VM_WRITE | SYNCHRONIZE,
                                    FALSE, explorer.pid_);
    return explorer;
}

Explorer::Explorer(Explorer&& other) noexcept
    : process_(std::exchange(other.process_, nullptr)), pid_(std::exchange(other.pid_, 0)) {}

Explorer& Explorer::operator=(Explorer&& other) noexcept {
    std::swap(process_, other.process_);
    std::swap(pid_, other.pid_);
    return *this;
}

Explorer::~Explorer() {
    if (process_) CloseHandle(process_);
}

DWORD Explorer::ExitCode() const {
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(process_, &code);
    return code;
}

bool Explorer::HasAltTabModules() const {
    bool twinui = false, switcher = false;
    for (HMODULE module : Modules()) {
        wchar_t name[MAX_PATH];
        if (!GetModuleBaseNameW(process_, module, name, MAX_PATH)) continue;
        twinui |= _wcsicmp(name, kTwinui) == 0;
        switcher |= _wcsicmp(name, kSwitcher) == 0;
    }
    return twinui && switcher;
}

static bool MarkerExists(const std::wstring& name) {
    HANDLE marker = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    if (marker) CloseHandle(marker);
    return marker != nullptr;
}

bool Explorer::IsHooked() const { return MarkerExists(protocol::HookedMarkerName(pid_)); }

bool Explorer::IsActive() const { return MarkerExists(protocol::ActiveMarkerName(pid_)); }

bool Explorer::HasLoaded(const std::wstring& dllPath) const { return FindModule(dllPath) != nullptr; }

std::optional<DWORD> Explorer::LoadAndStart(const std::wstring& dllPath) {
    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process_, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) return 0;
    if (!WriteProcessMemory(process_, remotePath, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(process_, remotePath, 0, MEM_RELEASE);
        return 0;
    }
    // If LoadLibraryW doesn't return in time it may still read the path: keep the buffer.
    if (!RunRemoteThread(Kernel32Function("LoadLibraryW"), remotePath)) return std::nullopt;
    VirtualFreeEx(process_, remotePath, 0, MEM_RELEASE);

    HMODULE module = FindModule(dllPath);
    if (!module) return 0;
    std::optional<DWORD> result = CallExport(module, dllPath, protocol::kStartExport);
    if (result && *result != protocol::kStarted) Unload(module);
    return result;
}

std::optional<DWORD> Explorer::StopAndUnload(const std::wstring& dllPath) {
    HMODULE module = FindModule(dllPath);
    if (!module) return protocol::kNotRunning;
    std::optional<DWORD> result = CallExport(module, dllPath, protocol::kStopExport);
    if (result && (*result == protocol::kStopped || *result == protocol::kNotRunning)) Unload(module);
    return result;
}

std::vector<HMODULE> Explorer::Modules() const {
    std::vector<HMODULE> modules(512);
    for (;;) {
        DWORD needed = 0;
        DWORD bytes = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
        if (!EnumProcessModulesEx(process_, modules.data(), bytes, &needed, LIST_MODULES_64BIT)) return {};
        if (needed <= bytes) {
            modules.resize(needed / sizeof(HMODULE));
            return modules;
        }
        modules.resize(needed / sizeof(HMODULE) + 64);
    }
}

HMODULE Explorer::FindModule(const std::wstring& path) const {
    for (HMODULE module : Modules()) {
        wchar_t name[MAX_PATH];
        if (GetModuleFileNameExW(process_, module, name, MAX_PATH) && _wcsicmp(name, path.c_str()) == 0) return module;
    }
    return nullptr;
}

std::optional<DWORD> Explorer::CallExport(HMODULE module, const std::wstring& dllPath, const char* name) {
    DWORD offset = ExportOffset(dllPath, name);
    if (!offset) return 0;
    return RunRemoteThread(reinterpret_cast<BYTE*>(module) + offset, nullptr);
}

std::optional<DWORD> Explorer::RunRemoteThread(void* start, void* parameter) {
    HANDLE thread =
        CreateRemoteThread(process_, nullptr, 0, static_cast<LPTHREAD_START_ROUTINE>(start), parameter, 0, nullptr);
    if (!thread) return 0;
    std::optional<DWORD> result;
    DWORD exitCode = 0;
    if (WaitForSingleObject(thread, kRemoteCallTimeoutMs) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &exitCode)) {
        result = exitCode;
    }
    CloseHandle(thread);
    return result;
}

void Explorer::Unload(HMODULE module) { RunRemoteThread(Kernel32Function("FreeLibrary"), module); }
