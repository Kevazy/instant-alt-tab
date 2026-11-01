// instant-alt-tab.exe: installs, removes, starts, stops and reports on Instant Alt+Tab.
// Double-clicked, it asks whether to install, update or uninstall.

#include "install.h"
#include "output.h"
#include "../agent/control.h"
#include "../agent/crash_guard.h"
#include "../agent/explorer.h"
#include "../shared/protocol.h"
#include "../shared/text_file.h"

#include <windows.h>

#include <cwctype>
#include <string>

namespace {

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory = path;
    return directory.substr(0, directory.rfind(L'\\') + 1);
}

// The directory whose agent and DLL a command acts on: the installed copy if
// there is one, otherwise the files next to this executable.
std::wstring ActiveDirectory() { return install::IsInstalled() ? install::InstallDirectory() : ExecutableDirectory(); }

void PrintUsage() {
    Print(L"Instant Alt+Tab " PROJECT_VERSION L": opens the Windows 11 Alt+Tab menu instantly.");
    Print(L"");
    Print(L"Usage: instant-alt-tab <command>");
    Print(L"");
    Print(L"  install     install for this user; starts now and with Windows");
    Print(L"  uninstall   remove the hooks from Explorer and delete the installed files");
    Print(L"  status      show whether it is installed, running and active in Explorer");
    Print(L"  start       start the agent (without installing, if not installed)");
    Print(L"  stop        stop the agent and remove the hooks until the next start or sign-in");
}

bool PrintStatus() {
    Explorer explorer = Explorer::Find();
    bool hooked = explorer.IsValid() && explorer.IsHooked();
    const wchar_t* altTab = !hooked               ? L"stock"
                            : explorer.IsActive() ? L"instant"
                                                  : L"stock (the hooks turned themselves off; see hook.log)";
    Print(L"Installed:          %s", install::IsInstalled() ? install::InstallDirectory().c_str() : L"no");
    Print(L"Starts at sign-in:  %s", install::StartsAtSignIn() ? L"yes" : L"no");
    Print(L"Agent:              %s", agent_control::IsRunning() ? L"running" : L"not running");
    Print(L"Alt+Tab:            %s", altTab);
    if (install::IsInstalled() && install::Disabled()) {
        Print(L"Turned off after Explorer crashed twice within an hour; install again to turn it back on.");
    }
    if (std::wstring problem = ReadTextFile(ActiveDirectory() + protocol::kProblemFile); !problem.empty()) {
        Print(L"Not active for this version of Windows: %s", problem.c_str());
    }
    return true;
}

bool Start() {
    std::wstring directory = ActiveDirectory();
    if (CrashGuard(directory).Disabled()) {
        Print(L"Turned off after Explorer crashed twice within an hour; install again to turn it back on.");
        return false;
    }
    if (!agent_control::IsRunning() && !agent_control::Launch(directory + protocol::kAgentExecutable)) {
        Print(L"Starting the agent failed (error %lu).", GetLastError());
        return false;
    }
    Print(L"Running. Alt+Tab opens at the usual speed once or twice while it sets up, then instantly.");
    return true;
}

bool Stop() {
    bool stopped = install::Stop(ActiveDirectory() + protocol::kHookDll);
    if (stopped) Print(L"Stopped. Alt+Tab is back to stock until the agent starts again.");
    return stopped;
}

bool Confirm(const wchar_t* question, bool byDefault) {
    Ask(L"%s %s ", question, byDefault ? L"[Y/n]" : L"[y/N]");
    wchar_t answer[16] = {};
    DWORD read = 0;
    ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), answer, 15, &read, nullptr);
    wchar_t first = read ? static_cast<wchar_t>(std::towlower(answer[0])) : L'\r';
    return first == L'\r' || first == L'\n' ? byDefault : first == L'y';
}

// Double-clicked, or started from Settings > Apps: the console window is this
// process's own and closes when it exits.
bool HasOwnConsole() {
    DWORD processes[2];
    return GetConsoleProcessList(processes, 2) == 1;
}

void WaitForEnter() {
    Print(L"");
    Print(L"Press Enter to close.");
    wchar_t ignored[4];
    DWORD read;
    ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), ignored, 3, &read, nullptr);
}

bool Interactive() {
    std::wstring here = ExecutableDirectory();
    if (!install::IsInstalled()) {
        Print(L"Instant Alt+Tab keeps the Alt+Tab menu ready, so it opens instantly instead of being rebuilt");
        Print(L"every time. It installs for this user only (no administrator rights) and starts with Windows.");
        Print(L"");
        return !Confirm(L"Install it now?", true) || install::Install(here);
    }
    PrintStatus();
    Print(L"");
    if (_wcsicmp(here.c_str(), install::InstallDirectory().c_str()) != 0) {
        return !Confirm(L"Replace the installed copy with this one?", true) || install::Install(here);
    }
    if (install::Disabled()) return !Confirm(L"Turn it back on?", true) || install::Install(here);
    return !Confirm(L"Uninstall Instant Alt+Tab?", false) || install::Uninstall(here);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring command = argc > 1 ? argv[1] : L"";
    bool ownConsole = HasOwnConsole();

    bool succeeded;
    if (command.empty() && ownConsole) {
        succeeded = Interactive();
    } else if (command == L"install") {
        succeeded = install::Install(ExecutableDirectory());
    } else if (command == L"uninstall") {
        succeeded = install::Uninstall(ExecutableDirectory());
    } else if (command == L"status") {
        succeeded = PrintStatus();
    } else if (command == L"start") {
        succeeded = Start();
    } else if (command == L"stop") {
        succeeded = Stop();
    } else {
        PrintUsage();
        succeeded = command.empty() || command == L"help" || command == L"--help" || command == L"-h";
    }
    if (ownConsole) WaitForEnter();  // otherwise the window closes before the result can be read
    return succeeded ? 0 : 1;
}
