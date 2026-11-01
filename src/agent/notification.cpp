#include "notification.h"

#include <windows.h>

#include <thread>

void Notify(const std::wstring& text) {
    // On top of other windows, but not forced into the foreground: Windows
    // normally doesn't give a background process the keyboard focus, so typing
    // elsewhere isn't interrupted, and the taskbar button flashes instead.
    std::thread([text] {
        MessageBoxW(nullptr, text.c_str(), L"Instant Alt+Tab", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }).detach();
}
