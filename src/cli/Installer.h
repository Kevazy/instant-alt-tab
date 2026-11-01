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

// Copies this executable to the install folder and writes the agent and the
// hook DLL it carries next to it.
bool Install();
bool Uninstall(const std::wstring& runningFrom);

// Stops a running agent, and removes the hooks of the copy at dllPath if an
// agent that was killed left them in Explorer.
bool StopAgentAndHooks(const std::wstring& dllPath);

}  // namespace Installer
