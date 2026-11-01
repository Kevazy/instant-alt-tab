#pragma once

// The shell's Explorer process: finding it, reading which modules it loaded,
// and loading, starting, stopping and unloading the hook DLL in it.

#include "../shared/internals.h"

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

class Explorer {
public:
    // The current shell Explorer, or an invalid object if there is none yet.
    static Explorer Find();

    Explorer() = default;
    Explorer(Explorer&& other) noexcept;
    Explorer& operator=(Explorer&& other) noexcept;
    ~Explorer();

    bool IsValid() const { return process_ != nullptr; }
    DWORD Pid() const { return pid_; }
    HANDLE Handle() const { return process_; }  // signaled when Explorer exits
    DWORD ExitCode() const;

    // Whether a module (by name) is loaded, the file it was loaded from, and the
    // identity of the loaded image.
    bool HasModule(const wchar_t* name) const;
    std::wstring ModulePath(const wchar_t* name) const;
    std::optional<internals::Module> LoadedImage(const wchar_t* name) const;
    // Whether some copy of the hook DLL is started, and whether it still keeps
    // the menu (it turns itself off if something unexpected happens).
    bool IsHooked() const;
    bool IsActive() const;
    // Whether this copy (by full path) is loaded.
    bool HasLoaded(const std::wstring& dllPath) const;

    // The results below are protocol::StartResult / StopResult values, 0 if the
    // DLL couldn't be loaded, or nullopt if a call into Explorer didn't return
    // in time. Its code may then still be running, so nothing is freed or
    // unloaded and that copy must be left alone until Explorer exits.

    // Loads a DLL of Windows' own that Explorer would load later anyway; it stays
    // loaded. Returns whether it is loaded now.
    std::optional<bool> Load(const std::wstring& dllPath);
    // Loads the DLL and calls Start with what discovery found; unloads it again
    // unless it started. Call it only when no copy is hooked, so loads and
    // unloads stay paired.
    std::optional<DWORD> LoadAndStart(const std::wstring& dllPath, const internals::Internals& found);
    // Calls Stop, then unloads the DLL unless it pinned itself.
    std::optional<DWORD> StopAndUnload(const std::wstring& dllPath);

private:
    std::vector<HMODULE> Modules() const;
    HMODULE FindModule(const std::wstring& path) const;
    HMODULE FindModuleByName(const wchar_t* name) const;
    std::optional<DWORD> CallExport(HMODULE module, const std::wstring& dllPath, const char* name,
                                    void* parameter = nullptr);
    std::optional<DWORD> RunRemoteThread(void* start, void* parameter);
    void Unload(HMODULE module);

    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
};
