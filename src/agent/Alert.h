#pragma once

// Tells the user something with a small message window that stays on top until
// they close it. Used only when Instant Alt+Tab stops working, so it's rare by
// design. Returns at once.

#include <string>

void ShowAlert(const std::wstring& text);
