#pragma once

// Tells the user something with a small message window that stays on top until
// they close it. Used only when Instant Alt+Tab stops working, so it's rare by
// design. Both return at once.

#include <string>

namespace Alert {

void Show(const std::wstring& text);

// Shows the message unless the user was already shown the one with this key
// (e.g. "paused <builds>"), remembered in a file in `directory`.
void ShowOnce(const std::wstring& directory, const std::wstring& key, const std::wstring& text);

}  // namespace Alert
