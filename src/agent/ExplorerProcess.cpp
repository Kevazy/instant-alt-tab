#include "ExplorerProcess.h"

#include "../shared/HookExports.h"
#include "../shared/SharedNames.h"

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

ExplorerProcess ExplorerProcess::Find() {
    ExplorerProcess explorer;
    HWND shell = GetShellWindow();
    if (!shell || !GetWindowThreadProcessId(shell, &explorer.m_pid)) return explorer;
    explorer.m_process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                         PROCESS_VM_READ | PROCESS_VM_WRITE | SYNCHRONIZE,
                                     FALSE, explorer.m_pid);
    return explorer;
}

ExplorerProcess::ExplorerProcess(ExplorerProcess&& other) noexcept
    : m_process(std::exchange(other.m_process, nullptr)), m_pid(std::exchange(other.m_pid, 0)) {}

ExplorerProcess& ExplorerProcess::operator=(ExplorerProcess&& other) noexcept {
    std::swap(m_process, other.m_process);
    std::swap(m_pid, other.m_pid);
    return *this;
}

ExplorerProcess::~ExplorerProcess() {
    if (m_process) CloseHandle(m_process);
}

DWORD ExplorerProcess::ExitCode() const {
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(m_process, &code);
    return code;
}

bool ExplorerProcess::HasModule(const wchar_t* name) const { return FindModuleByName(name) != nullptr; }

std::wstring ExplorerProcess::ModulePath(const wchar_t* name) const {
    wchar_t path[MAX_PATH];
    HMODULE module = FindModuleByName(name);
    return module && GetModuleFileNameExW(m_process, module, path, MAX_PATH) ? path : L"";
}

std::optional<AltTab::ImageIdentity> ExplorerProcess::LoadedImage(const wchar_t* name) const {
    auto base = reinterpret_cast<BYTE*>(FindModuleByName(name));
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!base || !ReadProcessMemory(m_process, base, &dos, sizeof(dos), nullptr) ||
        !ReadProcessMemory(m_process, base + dos.e_lfanew, &nt, sizeof(nt), nullptr)) {
        return std::nullopt;
    }
    return AltTab::ImageIdentity{nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage};
}

static bool MarkerExists(const std::wstring& name) {
    HANDLE marker = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    if (marker) CloseHandle(marker);
    return marker != nullptr;
}

bool ExplorerProcess::IsHooked() const { return MarkerExists(SharedNames::HookedMarkerName(m_pid)); }

bool ExplorerProcess::IsActive() const { return MarkerExists(SharedNames::ActiveMarkerName(m_pid)); }

bool ExplorerProcess::HasLoaded(const std::wstring& dllPath) const { return FindModule(dllPath) != nullptr; }

std::optional<bool> ExplorerProcess::Load(const std::wstring& dllPath) {
    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(m_process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) return false;
    if (!WriteProcessMemory(m_process, remotePath, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(m_process, remotePath, 0, MEM_RELEASE);
        return false;
    }
    // If LoadLibraryW doesn't return in time it may still read the path: keep the buffer.
    if (!RunRemoteThread(Kernel32Function("LoadLibraryW"), remotePath)) return std::nullopt;
    VirtualFreeEx(m_process, remotePath, 0, MEM_RELEASE);
    return FindModule(dllPath) != nullptr;
}

std::optional<DWORD> ExplorerProcess::LoadAndStart(const std::wstring& dllPath, const AltTab::Internals& found) {
    std::optional<bool> loaded = Load(dllPath);
    if (!loaded) return std::nullopt;
    HMODULE module = *loaded ? FindModule(dllPath) : nullptr;
    if (!module) return 0;
    void* remoteFound = VirtualAllocEx(m_process, nullptr, sizeof(found), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteFound || !WriteProcessMemory(m_process, remoteFound, &found, sizeof(found), nullptr)) {
        if (remoteFound) VirtualFreeEx(m_process, remoteFound, 0, MEM_RELEASE);
        Unload(module);
        return 0;
    }
    std::optional<DWORD> result = CallExport(module, dllPath, HookExports::kStartExport, remoteFound);
    if (!result) return result;  // Start may still read it: keep the copy
    VirtualFreeEx(m_process, remoteFound, 0, MEM_RELEASE);
    if (*result != HookExports::kStarted) Unload(module);
    return result;
}

std::optional<DWORD> ExplorerProcess::StopAndUnload(const std::wstring& dllPath) {
    HMODULE module = FindModule(dllPath);
    if (!module) return HookExports::kNotRunning;
    std::optional<DWORD> result = CallExport(module, dllPath, HookExports::kStopExport);
    if (result && (*result == HookExports::kStopped || *result == HookExports::kNotRunning)) Unload(module);
    return result;
}

std::vector<HMODULE> ExplorerProcess::Modules() const {
    std::vector<HMODULE> modules(512);
    for (;;) {
        DWORD needed = 0;
        DWORD bytes = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
        if (!EnumProcessModulesEx(m_process, modules.data(), bytes, &needed, LIST_MODULES_64BIT)) return {};
        if (needed <= bytes) {
            modules.resize(needed / sizeof(HMODULE));
            return modules;
        }
        modules.resize(needed / sizeof(HMODULE) + 64);
    }
}

HMODULE ExplorerProcess::FindModuleByName(const wchar_t* name) const {
    for (HMODULE module : Modules()) {
        wchar_t baseName[MAX_PATH];
        if (GetModuleBaseNameW(m_process, module, baseName, MAX_PATH) && _wcsicmp(baseName, name) == 0) return module;
    }
    return nullptr;
}

HMODULE ExplorerProcess::FindModule(const std::wstring& path) const {
    for (HMODULE module : Modules()) {
        wchar_t name[MAX_PATH];
        if (GetModuleFileNameExW(m_process, module, name, MAX_PATH) && _wcsicmp(name, path.c_str()) == 0) return module;
    }
    return nullptr;
}

std::optional<DWORD> ExplorerProcess::CallExport(HMODULE module, const std::wstring& dllPath, const char* name,
                                                 void* parameter) {
    DWORD offset = ExportOffset(dllPath, name);
    if (!offset) return 0;
    return RunRemoteThread(reinterpret_cast<BYTE*>(module) + offset, parameter);
}

std::optional<DWORD> ExplorerProcess::RunRemoteThread(void* start, void* parameter) {
    HANDLE thread =
        CreateRemoteThread(m_process, nullptr, 0, static_cast<LPTHREAD_START_ROUTINE>(start), parameter, 0, nullptr);
    if (!thread) return 0;
    std::optional<DWORD> result;
    DWORD exitCode = 0;
    if (WaitForSingleObject(thread, kRemoteCallTimeoutMs) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &exitCode)) {
        result = exitCode;
    }
    CloseHandle(thread);
    return result;
}

void ExplorerProcess::Unload(HMODULE module) { RunRemoteThread(Kernel32Function("FreeLibrary"), module); }
