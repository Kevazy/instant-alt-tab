#pragma once

// Per-user installation: no administrator rights and nothing outside the
// user's profile. The files go to %LOCALAPPDATA%\Programs\InstantAltTab, a Run
// entry starts the agent at sign-in, and an entry in Settings > Apps uninstalls it.

#include <string>

namespace install {

std::wstring InstallDirectory();  // with a trailing backslash
bool IsInstalled();
bool StartsAtSignIn();
bool Disabled();  // by the crash guard

bool Install(const std::wstring& sourceDirectory);
bool Uninstall(const std::wstring& runningFrom);

// Stops a running agent, and removes the hooks of the copy at dllPath if an
// agent that was killed left them in Explorer.
bool Stop(const std::wstring& dllPath);

}  // namespace install
