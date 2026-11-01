#pragma once

// Keeps the Alt+Tab menu alive between openings. Windows builds the whole
// menu (a XAML control with a card per window) on every Alt+Tab and throws it
// away when the menu closes; with many windows open that build is most of the
// delay. Here the control stays attached to its hidden host window and the
// next opening shows it again.

#include "Hooking.h"

namespace PersistentMenu {

bool InstallHooks(const ModuleImage& twinui, const ModuleImage& switcher, const AltTab::Internals& found);
void Enable();

// Drops the kept menu on Explorer's UI thread and stops keeping one. Returns
// false if the UI thread didn't confirm within the timeout.
bool Stop(DWORD timeoutMs);

}  // namespace PersistentMenu
