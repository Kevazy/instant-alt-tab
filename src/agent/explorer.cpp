#include "explorer.h"

#include "../shared/protocol.h"

#include <psapi.h>

#include <utility>

namespace {

// Stop can take up to ~15 s by itself (it waits for Explorer's UI thread and for hooked calls).
constexpr DWORD kRemoteCallTimeoutMs = 30000;

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

bool Explorer::HasModule(const wchar_t* name) const { return FindModuleByName(name) != nullptr; }

std::wstring Explorer::ModulePath(const wchar_t* name) const {
    wchar_t path[MAX_PATH];
    HMODULE module = FindModuleByName(name);
    return module && GetModuleFileNameExW(process_, module, path, MAX_PATH) ? path : L"";
}

std::optional<internals::Module> Explorer::LoadedImage(const wchar_t* name) const {
    auto base = reinterpret_cast<BYTE*>(FindModuleByName(name));
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!base || !ReadProcessMemory(process_, base, &dos, sizeof(dos), nullptr) ||
        !ReadProcessMemory(process_, base + dos.e_lfanew, &nt, sizeof(nt), nullptr)) {
        return std::nullopt;
    }
    return internals::Module{nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage};
}

static bool MarkerExists(const std::wstring& name) {
    HANDLE marker = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    if (marker) CloseHandle(marker);
    return marker != nullptr;
}

bool Explorer::IsHooked() const { return MarkerExists(protocol::HookedMarkerName(pid_)); }

bool Explorer::IsActive() const { return MarkerExists(protocol::ActiveMarkerName(pid_)); }

bool Explorer::HasLoaded(const std::wstring& dllPath) const { return FindModule(dllPath) != nullptr; }

std::optional<bool> Explorer::Load(const std::wstring& dllPath) {
    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process_, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) return false;
    if (!WriteProcessMemory(process_, remotePath, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(process_, remotePath, 0, MEM_RELEASE);
        return false;
    }
    // If LoadLibraryW doesn't return in time it may still read the path: keep the buffer.
    if (!RunRemoteThread(Kernel32Function("LoadLibraryW"), remotePath)) return std::nullopt;
    VirtualFreeEx(process_, remotePath, 0, MEM_RELEASE);
    return FindModule(dllPath) != nullptr;
}

std::optional<DWORD> Explorer::LoadAndStart(const std::wstring& dllPath, const internals::Internals& found) {
    std::optional<bool> loaded = Load(dllPath);
    if (!loaded) return std::nullopt;
    HMODULE module = *loaded ? FindModule(dllPath) : nullptr;
    if (!module) return 0;
    void* remoteFound = VirtualAllocEx(process_, nullptr, sizeof(found), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteFound || !WriteProcessMemory(process_, remoteFound, &found, sizeof(found), nullptr)) {
        if (remoteFound) VirtualFreeEx(process_, remoteFound, 0, MEM_RELEASE);
        Unload(module);
        return 0;
    }
    std::optional<DWORD> result = CallExport(module, dllPath, protocol::kStartExport, remoteFound);
    if (!result) return result;  // Start may still read it: keep the copy
    VirtualFreeEx(process_, remoteFound, 0, MEM_RELEASE);
    if (*result != protocol::kStarted) Unload(module);
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

HMODULE Explorer::FindModuleByName(const wchar_t* name) const {
    for (HMODULE module : Modules()) {
        wchar_t baseName[MAX_PATH];
        if (GetModuleBaseNameW(process_, module, baseName, MAX_PATH) && _wcsicmp(baseName, name) == 0) return module;
    }
    return nullptr;
}

HMODULE Explorer::FindModule(const std::wstring& path) const {
    for (HMODULE module : Modules()) {
        wchar_t name[MAX_PATH];
        if (GetModuleFileNameExW(process_, module, name, MAX_PATH) && _wcsicmp(name, path.c_str()) == 0) return module;
    }
    return nullptr;
}

std::optional<DWORD> Explorer::CallExport(HMODULE module, const std::wstring& dllPath, const char* name,
                                          void* parameter) {
    DWORD offset = ExportOffset(dllPath, name);
    if (!offset) return 0;
    return RunRemoteThread(reinterpret_cast<BYTE*>(module) + offset, parameter);
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
