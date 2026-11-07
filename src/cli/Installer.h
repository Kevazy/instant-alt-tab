#pragma once

// Per-user installation: no administrator rights and nothing outside the
// user's profile. The files go to %LOCALAPPDATA%\Programs\InstantAltTab, a Run
// entry starts the agent at sign-in, and an entry in Settings > Apps uninstalls it.

#include <string>

namespace Installer {

std::wstring InstallDirectory();  // with a trailing backslash
bool IsInstalled();
bool StartsAtSignIn();
bool DisabledByCrashGuard();
std::wstring InstalledVersion();  // empty if unknown

// Copies this executable to the install folder and writes the agent and the
// hook DLL it carries next to it. `openAltTab`: once ready, opens Alt+Tab once
// for the user (see FirstAltTab.h); only for an install the user just confirmed.
bool Install(bool openAltTab);
bool Uninstall(const std::wstring& runningFrom);

// After the agent in `directory` was started: shows its progress until the
// hooks are active in Explorer, the agent reports why they aren't, or it takes
// too long. Returns true once Alt+Tab is instant. `openAltTab`: see Install.
bool WaitUntilReady(const std::wstring& directory, bool openAltTab);

// Stops a running agent, and removes the hooks of the copy at dllPath if an
// agent that was killed left them in Explorer.
bool StopAgentAndHooks(const std::wstring& dllPath);

}  // namespace Installer
