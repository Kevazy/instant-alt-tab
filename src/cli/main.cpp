// instant-alt-tab.exe: installs, removes, starts, stops and reports on Instant Alt+Tab.
// Double-clicked, it asks whether to install, update or uninstall.

#include "Installer.h"
#include "AgentClient.h"
#include "Console.h"
#include "FirstAltTab.h"
#include "../agent/CrashGuard.h"
#include "../agent/ExplorerProcess.h"
#include "../shared/SharedNames.h"
#include "../shared/TextFile.h"

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
std::wstring ActiveDirectory() {
    return Installer::IsInstalled() ? Installer::InstallDirectory() : ExecutableDirectory();
}

void PrintUsage() {
    Print(L"<b>Instant Alt+Tab</> " PROJECT_VERSION L": opens the Windows 11 Alt+Tab menu instantly.");
    Print(L"");
    Print(L"Usage: instant-alt-tab <command>");
    Print(L"");
    Print(L"  install     install for this user; starts now and with Windows");
    Print(L"  uninstall   remove the hooks from Explorer and delete the installed files");
    Print(L"  status      show whether it is installed, running and active in Explorer");
    Print(L"  start       start the agent (without installing, if not installed)");
    Print(L"  stop        stop the agent and remove the hooks until the next start or sign-in");
}

// `forPrompt`: shown above a question that offers to turn it back on, so it doesn't say how.
bool PrintStatus(bool forPrompt = false) {
    ExplorerProcess explorer = ExplorerProcess::Find();
    bool hooked = explorer.IsValid() && explorer.IsHooked();
    bool instant = hooked && explorer.IsActive();
    const wchar_t* altTab = !hooked   ? L"normal speed"
                            : instant ? L"instant"
                                      : L"normal speed (the hooks turned themselves off; see hook.log)";
    bool installed = Installer::IsInstalled();
    Print(L"<dim>Installed:</>          %s", installed ? Installer::InstallDirectory().c_str() : L"no");
    Print(L"<dim>Starts at sign-in:</>  %s", Installer::StartsAtSignIn() ? L"yes" : L"no");
    Print(AgentClient::IsRunning() ? L"<dim>Agent:</>              <good>running</>"
                                   : L"<dim>Agent:</>              <warn>not running</>");
    Print(instant ? L"<dim>Alt+Tab:</>            <good>%s</>" : L"<dim>Alt+Tab:</>            <warn>%s</>", altTab);
    if (installed && Installer::DisabledByCrashGuard()) {
        Print(forPrompt ? L"<warn>Turned off after Explorer crashed twice within an hour.</>"
                        : L"<warn>Turned off after Explorer crashed twice within an hour;</> install again to turn "
                          L"it back on.");
    }
    if (std::wstring problem = ReadTextFile(ActiveDirectory() + SharedNames::kProblemFile); !problem.empty()) {
        Print(L"<dim>Why not instant:</>    %s", problem.c_str());
    }
    return true;
}

bool Start() {
    std::wstring directory = ActiveDirectory();
    if (CrashGuard(directory).Disabled()) {
        Print(L"<warn>Turned off after Explorer crashed twice within an hour;</> install again to turn it back on.");
        return false;
    }
    std::wstring agent = directory + SharedNames::kAgentExecutable;
    if (GetFileAttributesW(agent.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Print(L"<warn>Not installed:</> run instant-alt-tab without a command to install it.");
        return false;
    }
    if (AgentClient::IsRunning()) {
        ExplorerProcess explorer = ExplorerProcess::Find();
        if (explorer.IsValid() && explorer.IsActive()) {
            if (FirstAltTab::IsKept(explorer.Pid()).value_or(true)) {  // older hooks don't say
                Print(L"<good>Already running;</> Alt+Tab opens instantly.");
            } else {
                Print(L"<good>Already running.</> The next Alt+Tab opens at normal speed while it builds the menu,");
                Print(L"and every one after that opens instantly.");
            }
            return true;
        }
    } else {
        DeleteFileW((directory + SharedNames::kProblemFile).c_str());  // from an earlier run
        if (!AgentClient::Launch(agent)) {
            Print(L"<bad>Starting the agent failed</> (error %lu).", GetLastError());
            return false;
        }
    }
    return Installer::WaitUntilReady(directory, /*openAltTab*/ false);
}

bool Stop() {
    bool stopped = Installer::StopAgentAndHooks(ActiveDirectory() + SharedNames::kHookDll);
    if (stopped) Print(L"<good>Stopped.</> Alt+Tab is back to normal speed until the agent starts again.");
    return stopped;
}

// On yes, clears the window, so what follows is all that's on screen.
bool Confirm(const wchar_t* question, bool byDefault) {
    Ask(L"<b>%s</> <dim>%s</> ", question, byDefault ? L"[Y/n]" : L"[y/N]");
    wchar_t answer[16] = {};
    DWORD read = 0;
    ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), answer, 15, &read, nullptr);
    wchar_t first = read ? static_cast<wchar_t>(std::towlower(answer[0])) : L'\r';
    bool yes = first == L'\r' || first == L'\n' ? byDefault : first == L'y';
    if (yes) ClearScreen();
    return yes;
}

// Double-clicked, or started from Settings > Apps: the console window is this
// process's own and closes when it exits.
bool HasOwnConsole() {
    DWORD processes[2];
    return GetConsoleProcessList(processes, 2) == 1;
}

void WaitForEnter() {
    Print(L"");
    Print(L"<dim>Press Enter to close.</>");
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));  // an Enter pressed while it was busy
    wchar_t ignored[4];
    DWORD read;
    ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), ignored, 3, &read, nullptr);
}

enum class Outcome { Declined, Succeeded, Failed };

Outcome Interactive() {
    auto done = [](bool succeeded) { return succeeded ? Outcome::Succeeded : Outcome::Failed; };
    std::wstring here = ExecutableDirectory();
    if (!Installer::IsInstalled()) {
        Print(L"<b>Instant Alt+Tab</> keeps the Alt+Tab menu ready, so it opens instantly instead of being rebuilt");
        Print(L"every time. It installs for this user only and starts with Windows.");
        Print(L"");
        return Confirm(L"Install it now?", true) ? done(Installer::Install(/*openAltTab*/ true)) : Outcome::Declined;
    }
    PrintStatus(/*forPrompt*/ true);
    Print(L"");
    if (_wcsicmp(here.c_str(), Installer::InstallDirectory().c_str()) != 0) {
        std::wstring installed = Installer::InstalledVersion();
        std::wstring question = installed == PROJECT_VERSION ? L"Reinstall version " PROJECT_VERSION
                                : installed.empty() ? L"Replace the installed copy with version " PROJECT_VERSION
                                                    : L"Replace version " + installed + L" with " PROJECT_VERSION;
        question += Installer::DisabledByCrashGuard() ? L"? This also turns it back on." : L"?";
        return Confirm(question.c_str(), true) ? done(Installer::Install(/*openAltTab*/ true)) : Outcome::Declined;
    }
    if (Installer::DisabledByCrashGuard()) {
        return Confirm(L"Turn it back on?", true) ? done(Installer::Install(/*openAltTab*/ true)) : Outcome::Declined;
    }
    return Confirm(L"Uninstall Instant Alt+Tab?", false) ? done(Installer::Uninstall(here)) : Outcome::Declined;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring command = argc > 1 ? argv[1] : L"";
    bool ownConsole = HasOwnConsole();

    bool succeeded;
    if (command.empty() && ownConsole) {
        Outcome outcome = Interactive();
        if (outcome == Outcome::Declined) return 0;  // nothing happened, so nothing to read: close right away
        succeeded = outcome == Outcome::Succeeded;
    } else if (command == L"install") {
        succeeded = Installer::Install(/*openAltTab*/ false);
    } else if (command == L"uninstall") {
        succeeded = Installer::Uninstall(ExecutableDirectory());
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
