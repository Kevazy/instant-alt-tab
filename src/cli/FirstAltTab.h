#pragma once

// Right after installing, the installer opens Alt+Tab once itself, so the menu
// the hooks keep is built and checked before the user's own first press, which
// is then instant too.

#include <windows.h>

#include <optional>

namespace FirstAltTab {

// Whether the next Alt+Tab in that Explorer shows the kept menu, as far as the hooks can tell;
// nothing if the hooks running there are a version that doesn't say.
std::optional<bool> IsKept(DWORD explorerPid);

// Whether Alt+Tab can be pressed for the user now: the console window is in
// front, so the keys can't act on another window, and no key or mouse button is
// held, so they can't combine with the user's own.
bool CanOpenNow();

// Presses Alt+Tab, waits until the menu is on screen, and cancels it with Esc,
// which returns to the console (releasing Alt instead would switch windows).
// Returns whether the hooks kept the menu.
bool Open(DWORD explorerPid);

}  // namespace FirstAltTab
