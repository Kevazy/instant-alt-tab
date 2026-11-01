#include "Installer.h"

#include "AgentClient.h"
#include "Console.h"
#include "../agent/CrashGuard.h"
#include "../agent/ExplorerProcess.h"
#include "../shared/Ipc.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>

namespace Installer {
namespace {

constexpr wchar_t kCliExecutable[] = L"instant-alt-tab.exe";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunOnceKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
constexpr wchar_t kRunValue[] = L"InstantAltTab";
constexpr wchar_t kCleanupValue[] = L"InstantAltTabCleanup";
// Lists Instant Alt+Tab in Settings > Apps, so it can be uninstalled from there.
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\InstantAltTab";
constexpr DWORD kStopTimeoutMs = 30000;
constexpr DWORD kCopyRetryMs = 5000;  // a just-stopped agent may still be unmapping its executable

std::wstring AgentCommand() { return L"\"" + InstallDirectory() + Ipc::kAgentExecutable + L"\""; }

std::wstring SystemDirectory() {
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    return system;
}

bool SetValue(const wchar_t* key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool SetValue(const wchar_t* key, const wchar_t* name, DWORD value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

bool CopyWithRetry(const std::wstring& from, const std::wstring& to) {
    for (DWORD waited = 0;; waited += 250) {
        if (CopyFileW(from.c_str(), to.c_str(), FALSE)) {
            // A copy of a downloaded file keeps its "downloaded from the internet"
            // mark, which can make Windows ask before running it at sign-in.
            DeleteFileW((to + L":Zone.Identifier").c_str());
            return true;
        }
        if (GetLastError() != ERROR_SHARING_VIOLATION || waited >= kCopyRetryMs) return false;
        Sleep(250);
    }
}

bool RegisterUninstaller(const std::wstring& target) {
    std::wstring cli = target + kCliExecutable;
    return SetValue(kUninstallKey, L"DisplayName", std::wstring(L"Instant Alt+Tab")) &&
           SetValue(kUninstallKey, L"DisplayVersion", std::wstring(PROJECT_VERSION)) &&
           SetValue(kUninstallKey, L"DisplayIcon", cli) && SetValue(kUninstallKey, L"InstallLocation", target) &&
           SetValue(kUninstallKey, L"UninstallString", L"\"" + cli + L"\" uninstall") &&
           SetValue(kUninstallKey, L"NoModify", DWORD{1}) && SetValue(kUninstallKey, L"NoRepair", DWORD{1});
}

// Deletes the folder at the next sign-in, for files that are still in use now.
void DeleteAtNextSignIn(const std::wstring& folder) {
    SetValue(kRunOnceKey, kCleanupValue, L"\"" + SystemDirectory() + L"\\cmd.exe\" /d /c rd /s /q \"" + folder + L"\"");
}

// This executable is in the folder it deletes: a detached cmd.exe retries every
// second for ten minutes (the console may be waiting for Enter), and once the
// folder is gone it removes the sign-in fallback again.
void DeleteAfterExit(const std::wstring& folder) {
    std::wstring system = SystemDirectory();
    std::wstring removeFallback =
        L"\"" + system + L"\\reg.exe\" delete \"HKCU\\" + kRunOnceKey + L"\" /v " + kCleanupValue + L" /f >nul 2>&1";
    std::wstring commandLine = L"\"" + system + L"\\cmd.exe\" /d /c for /l %i in (1,1,600) do @(rd /s /q \"" + folder +
                               L"\" 2>nul & if not exist \"" + folder + L"\" (" + removeFallback +
                               L" & exit /b 0) & ping -n 2 127.0.0.1 >nul)";
    STARTUPINFOW startup{.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    if (CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, system.c_str(),
                       &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

}  // namespace

std::wstring InstallDirectory() {
    PWSTR programs = nullptr;  // %LOCALAPPDATA%\Programs
    std::wstring directory;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserProgramFiles, KF_FLAG_CREATE, nullptr, &programs))) {
        directory = std::wstring(programs) + L"\\InstantAltTab\\";
    }
    CoTaskMemFree(programs);
    return directory;
}

bool IsInstalled() {
    return GetFileAttributesW((InstallDirectory() + Ipc::kAgentExecutable).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool StartsAtSignIn() {
    wchar_t value[MAX_PATH * 2];
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS &&
           _wcsicmp(value, AgentCommand().c_str()) == 0;
}

bool Disabled() { return CrashGuard(InstallDirectory()).Disabled(); }

bool StopAgentAndHooks(const std::wstring& dllPath) {
    if (!AgentClient::RequestStop(kStopTimeoutMs)) {
        Print(L"The agent didn't stop within %lu seconds.", kStopTimeoutMs / 1000);
        return false;
    }
    ExplorerProcess explorer = ExplorerProcess::Find();
    if (explorer.IsValid() && explorer.HasLoaded(dllPath)) {
        std::optional<DWORD> result = explorer.StopAndUnload(dllPath);
        if (!result || explorer.HasLoaded(dllPath)) {
            Print(L"The hooks are off, but Explorer keeps %s loaded until it restarts (or you sign out).",
                  Ipc::kHookDll);
            return false;
        }
    }
    return true;
}

bool Install(const std::wstring& sourceDirectory) {
    std::wstring target = InstallDirectory();
    if (target.empty()) {
        Print(L"Couldn't find the folder for per-user programs (%%LOCALAPPDATA%%\\Programs).");
        return false;
    }
    if (!StopAgentAndHooks(target + Ipc::kHookDll)) {
        Print(L"Couldn't stop the running copy: sign out and in (or restart Explorer), then install again.");
        return false;
    }
    if (_wcsicmp(sourceDirectory.c_str(), target.c_str()) != 0) {
        CreateDirectoryW(target.c_str(), nullptr);
        for (const wchar_t* file : {kCliExecutable, Ipc::kAgentExecutable, Ipc::kHookDll}) {
            if (!CopyWithRetry(sourceDirectory + file, target + file)) {
                Print(L"Copying %s to %s failed (error %lu).", file, target.c_str(), GetLastError());
                return false;
            }
        }
    }
    CrashGuard(target).Clear();
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunOnceKey, kCleanupValue);  // from an earlier uninstall
    if (!SetValue(kRunKey, kRunValue, AgentCommand())) {
        Print(L"Registering the agent to start at sign-in failed.");
        return false;
    }
    if (!RegisterUninstaller(target)) Print(L"Adding Instant Alt+Tab to Settings > Apps failed; it works anyway.");
    if (!AgentClient::Launch(target + Ipc::kAgentExecutable)) {
        Print(L"Installed, but starting the agent failed (error %lu); it starts at the next sign-in.", GetLastError());
        return false;
    }
    Print(L"Installed to %s", target.c_str());
    Print(L"Instant Alt+Tab is running and starts with Windows. Alt+Tab opens at the usual speed once or");
    Print(L"twice while it sets up, then instantly. Uninstall it from Settings > Apps.");
    return true;
}

bool Uninstall(const std::wstring& runningFrom) {
    std::wstring target = InstallDirectory();
    bool stopped = StopAgentAndHooks(target + Ipc::kHookDll);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
    if (target.empty() || GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Print(stopped ? L"Not installed." : L"Not installed; the agent couldn't be stopped.");
        return stopped;
    }
    std::wstring folder = target.substr(0, target.size() - 1);  // without the trailing backslash
    if (_wcsicmp(runningFrom.c_str(), target.c_str()) == 0) {
        DeleteAfterExit(folder);
        DeleteAtNextSignIn(folder);  // in case it's still in use when the retries run out
        Print(L"Uninstalled. %s is deleted in a moment.", folder.c_str());
        return stopped;
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    if (error) {
        DeleteAtNextSignIn(folder);
        Print(L"Partly uninstalled: some files in %s are still in use, so the folder is deleted at the next sign-in.",
              folder.c_str());
        return false;
    }
    Print(L"Uninstalled: the hooks are removed from Explorer and %s is deleted.", folder.c_str());
    return stopped;
}

}  // namespace Installer
