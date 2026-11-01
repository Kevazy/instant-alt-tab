#pragma once

// The shell's Explorer process: finding it, waiting until its Alt+Tab modules
// are loaded, and loading, starting, stopping and unloading the hook DLL in it.

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

    // twinui and the switcher are loaded once Alt+Tab is first used.
    bool HasAltTabModules() const;
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

    // Loads the DLL and calls Start; unloads it again unless it started. Call it
    // only when no copy is hooked, so loads and unloads stay paired.
    std::optional<DWORD> LoadAndStart(const std::wstring& dllPath);
    // Calls Stop, then unloads the DLL unless it pinned itself.
    std::optional<DWORD> StopAndUnload(const std::wstring& dllPath);

private:
    std::vector<HMODULE> Modules() const;
    HMODULE FindModule(const std::wstring& path) const;
    std::optional<DWORD> CallExport(HMODULE module, const std::wstring& dllPath, const char* name);
    std::optional<DWORD> RunRemoteThread(void* start, void* parameter);
    void Unload(HMODULE module);

    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
};
