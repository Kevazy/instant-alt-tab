#include "Installer.h"

#include "AgentClient.h"
#include "Console.h"
#include "FirstAltTab.h"
#include "../agent/CrashGuard.h"
#include "../agent/ExplorerProcess.h"
#include "../shared/SharedNames.h"
#include "../shared/TextFile.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
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
constexpr DWORD kReadyPollMs = 250;
constexpr ULONGLONG kAgentStartMs = 5000;      // until a just-launched agent has created its mutex
constexpr ULONGLONG kReadyTimeoutMs = 180000;  // without progress: a stalled download, or an agent that hangs
constexpr ULONGLONG kHooksStartMs = 5000;      // from the Hooked marker to the Active one
constexpr ULONGLONG kDotEveryMs = 2000;
constexpr DWORD kFocusPollMs = 100;
constexpr ULONGLONG kInFrontForMs = 300;    // back in front and settled, not mid-click
constexpr ULONGLONG kHeldGiveUpMs = 10000;  // in front, but a key reads as held (stuck, or a remapping tool)
constexpr DWORD kTurnOffMs = 250;           // the hooks turn off from a message posted to Explorer's UI thread
constexpr int kLineWidth = 90;              // label and dots, leaving room for " ready." in a 100-column window
constexpr int kReadyRoom = 8;               // " ready." after the dots, and the cursor

std::wstring AgentCommand() { return L"\"" + InstallDirectory() + SharedNames::kAgentExecutable + L"\""; }

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

std::wstring ExecutablePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

// Retries `write` while the file is still mapped by a process that just exited.
template <typename Write>
bool WithRetry(Write write) {
    for (DWORD waited = 0;; waited += 250) {
        if (write()) return true;
        if (GetLastError() != ERROR_SHARING_VIOLATION || waited >= kCopyRetryMs) return false;
        Sleep(250);
    }
}

bool CopyExecutable(const std::wstring& from, const std::wstring& to) {
    if (!WithRetry([&] { return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE; })) return false;
    // A copy of a downloaded file keeps its "downloaded from the internet"
    // mark, which can make Windows ask before running it at sign-in.
    DeleteFileW((to + L":Zone.Identifier").c_str());
    return true;
}

// The agent and the hook DLL are built into this executable (see CMakeLists.txt),
// as resources named after their files, so a single downloaded file installs everything.
bool WriteEmbeddedFile(const wchar_t* name, const std::wstring& to) {
    HRSRC resource = FindResourceW(nullptr, name, RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) return false;
    DWORD size = SizeofResource(nullptr, resource);
    return WithRetry([&] {
        HANDLE file = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool complete = WriteFile(file, data, size, &written, nullptr) && written == size;
        CloseHandle(file);
        return complete;
    });
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

bool EventIsSet(const std::wstring& name) {
    HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    bool set = event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    if (event) CloseHandle(event);
    return set;
}

// Whether the agent is downloading symbols: discovery keeps the PDB there only
// while it downloads and reads it, and deletes it afterwards, also on failure.
bool Downloading(const std::wstring& directory) {
    std::wstring pattern = directory + SharedNames::kSymbolsFolder + SharedNames::kSymbolsDownloadFolder + L"*.pdb";
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    FindClose(search);
    return true;
}

struct Download {
    std::wstring file;
    int percent = -1;  // -1 while its size isn't known
    long long megabytes = 0;
    bool operator==(const Download&) const = default;
};

// The PDB the agent is downloading, and how much of it is in (its size file
// can be missing, or briefly empty).
std::optional<Download> CurrentDownload(const std::wstring& directory) {
    std::wstring folder = directory + SharedNames::kSymbolsFolder + SharedNames::kSymbolsDownloadFolder;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"*.pdb").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return std::nullopt;
    FindClose(search);
    std::wstring pdb = folder + found.cFileName;
    // The directory entry's size lags behind a file being written; the open file's doesn't.
    LARGE_INTEGER have{};
    HANDLE file = CreateFileW(pdb.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        GetFileSizeEx(file, &have);
        CloseHandle(file);
    }
    long long total = _wtoi64(ReadTextFile(pdb + SharedNames::kDownloadSizeSuffix).c_str());
    Download download{found.cFileName};
    download.megabytes = have.QuadPart >> 20;
    if (total > 0) download.percent = static_cast<int>(std::min<long long>(100, have.QuadPart * 100 / total));
    return download;
}

// Room for a step's label and its dots: within the window, so the line never
// wraps (a wrapped line can't be cleared and written again).
int LineWidth() {
    int columns = ConsoleColumns();
    return columns > 0 ? std::min(kLineWidth, columns - kReadyRoom) : kLineWidth;
}

void PrintTurnedOff(const std::wstring& directory) {
    Print(L"<warn>Instant Alt+Tab turned itself off:</> something in Explorer didn't behave as expected, so");
    Print(L"Alt+Tab stays at normal speed until Explorer restarts.");
    Print(L"<dim>Details: %s%s</>", directory.c_str(), SharedNames::kHookLog);
}

// Waits, for as long as the window stays open, until Alt+Tab can be pressed for
// the user: keys can only go to the console's own window, so if the user has
// switched away, it waits until they're back (their own Alt+Tab back builds the
// menu too). Returns whether the menu is kept.
bool OpenAltTabOnce(const ExplorerProcess& explorer) {
    for (ULONGLONG inFrontSince = 0, heldSince = 0;; Sleep(kFocusPollMs)) {
        if (FirstAltTab::IsKept(explorer.Pid()).value_or(false)) return true;
        if (!explorer.IsActive()) return false;
        if (!FirstAltTab::CanOpenNow()) {
            inFrontSince = 0;
            if (!FirstAltTab::InFront()) {
                heldSince = 0;
            } else if (!heldSince) {
                heldSince = GetTickCount64();
            } else if (GetTickCount64() - heldSince >= kHeldGiveUpMs) {
                return false;
            }
        } else if (!inFrontSince) {
            inFrontSince = GetTickCount64();
        } else if (GetTickCount64() - inFrontSince >= kInFrontForMs) {
            return FirstAltTab::Open(explorer.Pid());
        }
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
    return GetFileAttributesW((InstallDirectory() + SharedNames::kAgentExecutable).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool StartsAtSignIn() {
    wchar_t value[MAX_PATH * 2];
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS &&
           _wcsicmp(value, AgentCommand().c_str()) == 0;
}

bool DisabledByCrashGuard() { return CrashGuard(InstallDirectory()).Disabled(); }

std::wstring InstalledVersion() {
    wchar_t version[64];
    DWORD size = sizeof(version);
    return RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, version, &size) ==
                   ERROR_SUCCESS
               ? version
               : L"";
}

bool StopAgentAndHooks(const std::wstring& dllPath) {
    if (!AgentClient::RequestStop(kStopTimeoutMs)) {
        Print(L"<bad>The agent didn't stop within %lu seconds.</>", kStopTimeoutMs / 1000);
        return false;
    }
    ExplorerProcess explorer = ExplorerProcess::Find();
    if (explorer.IsValid() && explorer.HasLoaded(dllPath)) {
        std::optional<DWORD> result = explorer.StopAndUnload(dllPath);
        if (!result || explorer.HasLoaded(dllPath)) {
            Print(L"<warn>The hooks are off, but Explorer keeps %s loaded until it restarts (or you sign out).</>",
                  SharedNames::kHookDll);
            return false;
        }
    }
    return true;
}

bool Install(bool openAltTab) {
    std::wstring target = InstallDirectory();
    if (target.empty()) {
        Print(L"<bad>Couldn't find the folder for per-user programs (%%LOCALAPPDATA%%\\Programs).</>");
        return false;
    }
    bool updating = IsInstalled(), wasDisabled = updating && DisabledByCrashGuard();
    bool sameVersion = InstalledVersion() == PROJECT_VERSION;
    if (!StopAgentAndHooks(target + SharedNames::kHookDll)) {
        Print(L"<bad>Couldn't stop the running copy:</> sign out and in (or restart Explorer), then install again.");
        return false;
    }
    CreateDirectoryW(target.c_str(), nullptr);
    std::wstring self = ExecutablePath();
    if (_wcsicmp(self.c_str(), (target + kCliExecutable).c_str()) != 0 &&
        !CopyExecutable(self, target + kCliExecutable)) {
        Print(L"<bad>Copying %s to %s failed</> (error %lu).", kCliExecutable, target.c_str(), GetLastError());
        return false;
    }
    for (const wchar_t* file : {SharedNames::kAgentExecutable, SharedNames::kHookDll}) {
        if (!WriteEmbeddedFile(file, target + file)) {
            Print(L"<bad>Writing %s to %s failed</> (error %lu).", file, target.c_str(), GetLastError());
            return false;
        }
    }
    CrashGuard(target).Clear();
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunOnceKey, kCleanupValue);  // from an earlier uninstall
    if (!SetValue(kRunKey, kRunValue, AgentCommand())) {
        Print(L"<bad>Registering the agent to start at sign-in failed.</>");
        return false;
    }
    if (!RegisterUninstaller(target))
        Print(L"<warn>Adding Instant Alt+Tab to Settings > Apps failed;</> it works anyway.");
    DeleteFileW((target + SharedNames::kProblemFile).c_str());  // from an earlier run
    if (!AgentClient::Launch(target + SharedNames::kAgentExecutable)) {
        Print(L"<warn>Installed, but starting the agent failed</> (error %lu); it starts at the next sign-in.",
              GetLastError());
        return false;
    }
    if (updating && sameVersion) {
        Print(wasDisabled ? L"<good>Turned back on.</>" : L"<good>Reinstalled version " PROJECT_VERSION L".</>");
    } else if (updating) {
        Print(L"<good>Updated to version %s%s</>", PROJECT_VERSION, wasDisabled ? L", and turned back on." : L".");
    } else {
        Print(L"<good>Installed</> to %s.", target.substr(0, target.size() - 1).c_str());
        Print(L"<dim>Uninstall it any time from Settings > Apps.</>");
    }
    Print(L"");
    WaitUntilReady(target, openAltTab);  // installed either way: the agent carries on in the background
    return true;
}

bool WaitUntilReady(const std::wstring& directory, bool openAltTab) {
    std::wstring problemFile = directory + SharedNames::kProblemFile;
    enum class Phase { SettingUp, WaitingForExplorer, Downloading };
    Phase phase = Phase::SettingUp;
    const wchar_t* label = L"Setting up";
    int dots = 0;
    bool agentSeen = false, explainedDownload = false;
    std::optional<Download> progress;  // the download's progress, shown in place of the step's line
    // The timeout counts from the last sign of progress, so a slow but moving download isn't cut off.
    ULONGLONG started = GetTickCount64(), lastDot = started, hookedSince = 0, progressedAt = started;
    Ask(L"%s...", label);
    for (;; Sleep(kReadyPollMs)) {
        ULONGLONG waited = GetTickCount64() - started;
        ExplorerProcess explorer = ExplorerProcess::Find();
        bool hooked = explorer.IsValid() && explorer.IsHooked();
        if (!hooked) hookedSince = 0;
        if (hooked) {
            if (!hookedSince) hookedSince = GetTickCount64();
            if (explorer.IsActive()) {
                // The progress line becomes the step's own again.
                if (progress && ClearLine()) Ask(L"%s...", label);
                Print(L" <good>ready.</>");
                bool kept = FirstAltTab::IsKept(explorer.Pid()).value_or(false);
                if (!kept && openAltTab) {
                    bool canOpen = FirstAltTab::CanOpenNow();
                    Ask(canOpen ? L"Opening Alt+Tab once to warm it up..."
                                : L"Opening Alt+Tab once to warm it up, as soon as this window is in front again...");
                    kept = OpenAltTabOnce(explorer);
                    Sleep(kTurnOffMs);  // if that opening turned the hooks off, it shows by now
                    if (EventIsSet(SharedNames::TurnedOffEventName(explorer.Pid()))) {
                        Print(L"");
                        PrintTurnedOff(directory);
                        return false;
                    }
                    Print(kept ? L" <good>done.</>" : L" skipped.");
                }
                Print(L"");
                if (kept) {
                    Print(L"<b><good>Alt+Tab opens instantly now. Give it a try.</>");
                } else {
                    Print(L"<b>The first Alt+Tab opens at normal speed while it builds the menu, and every one after");
                    Print(L"that opens instantly.</>");
                }
                return true;
            }
            // Between the two markers while the hooks start; the event is set only when they turn themselves
            // off, and starting takes well under a second.
            if (EventIsSet(SharedNames::TurnedOffEventName(explorer.Pid())) ||
                GetTickCount64() - hookedSince >= kHooksStartMs) {
                Print(L"");
                PrintTurnedOff(directory);
                return false;
            }
        }
        if (std::wstring problem = ReadTextFile(problemFile); !problem.empty()) {
            Print(L"");
            std::wstring_view retrying = SharedNames::kProblemRetryingDownload;
            if (problem.starts_with(retrying)) {
                Print(
                    L"<warn>Couldn't download the symbol files from Microsoft yet.</> It keeps trying in the "
                    L"background");
                Print(L"and speeds Alt+Tab up as soon as the download works.");
                Print(L"<dim>Details: %s</>", problem.substr(retrying.size()).c_str());
            } else {
                Print(L"<warn>Alt+Tab stays at normal speed.</>");
                Print(L"<dim>Details: %s</>", problem.c_str());
            }
            return false;
        }
        agentSeen = agentSeen || AgentClient::IsRunning();
        if (!AgentClient::IsRunning() && (agentSeen || waited >= kAgentStartMs)) {
            Print(L"");
            if (CrashGuard(directory).Disabled()) {
                Print(
                    L"<warn>Turned off after Explorer crashed twice within an hour;</> install again to turn it back "
                    L"on.");
            } else {
                Print(L"<bad>The agent stopped while setting up.</>");
                Print(L"<dim>Details: %s%s</>", directory.c_str(), SharedNames::kAgentLog);
            }
            return false;
        }
        if (GetTickCount64() - progressedAt >= kReadyTimeoutMs) {
            Print(L"");
            Print(L"<warn>Still setting up.</> It carries on in the background; Alt+Tab speeds up once it's done.");
            return false;
        }
        Phase now = !explorer.IsValid()                                     ? Phase::WaitingForExplorer
                    : Downloading(directory) || phase == Phase::Downloading ? Phase::Downloading
                                                                            : Phase::SettingUp;
        if (now != phase) {
            phase = now;
            dots = 0;
            lastDot = GetTickCount64();
            // The new step replaces the old one's line where the console allows it.
            if (!ClearLine()) Print(L"");
            if (phase == Phase::Downloading && !explainedDownload) {
                explainedDownload = true;
                Print(L"<dim>This is the first run on this version of Windows: it downloads Alt+Tab's symbol files");
                Print(L"from Microsoft, usually in under a minute.</>");
                Print(L"");
            }
            label = phase == Phase::Downloading          ? L"Downloading"
                    : phase == Phase::WaitingForExplorer ? L"Waiting for Explorer (the taskbar) to start"
                                                         : L"Setting up";
            Ask(L"%s...", label);
        } else if (auto download = phase == Phase::Downloading ? CurrentDownload(directory) : std::nullopt;
                   download && download != progress && ClearLine()) {
            if (download->percent < 0) {
                Ask(L"Downloading %s... <good>%lld MB</>", download->file.c_str(), download->megabytes);
            } else {
                Ask(L"Downloading %s... <good>%d%%</>", download->file.c_str(), download->percent);
            }
            progress = download;
            progressedAt = GetTickCount64();
        } else if (!progress && GetTickCount64() - lastDot >= kDotEveryMs &&
                   dots < LineWidth() - static_cast<int>(wcslen(label)) - 3) {
            Ask(L"<dim>.</>");
            ++dots;
            lastDot = GetTickCount64();
        }
    }
}

bool Uninstall(const std::wstring& runningFrom) {
    std::wstring target = InstallDirectory();
    bool stopped = StopAgentAndHooks(target + SharedNames::kHookDll);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
    if (target.empty() || GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Print(stopped ? L"Not installed." : L"Not installed; <bad>the agent couldn't be stopped.</>");
        return stopped;
    }
    std::wstring folder = target.substr(0, target.size() - 1);  // without the trailing backslash
    if (_wcsicmp(runningFrom.c_str(), target.c_str()) == 0) {
        DeleteAfterExit(folder);
        DeleteAtNextSignIn(folder);  // in case it's still in use when the retries run out
        if (stopped) {
            Print(L"<good>Uninstalled.</> %s is deleted in a moment.", folder.c_str());
        } else {
            Print(L"<warn>Partly uninstalled:</> Explorer still has a file of it loaded, so %s is deleted once",
                  folder.c_str());
            Print(L"Explorer restarts, or at the next sign-in.");
        }
        return stopped;
    }
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    if (error) {
        DeleteAtNextSignIn(folder);
        Print(
            L"<warn>Partly uninstalled:</> some files in %s are still in use, so the folder is deleted at the next "
            L"sign-in.",
            folder.c_str());
        return false;
    }
    Print(L"<good>Uninstalled:</> the hooks are removed from Explorer and %s is deleted.", folder.c_str());
    return stopped;
}

}  // namespace Installer
