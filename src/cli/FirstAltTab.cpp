#include "FirstAltTab.h"

#include "../shared/SharedNames.h"

#include <atomic>
#include <iterator>
#include <string>

namespace FirstAltTab {
namespace {

// Alt is down for this long at most, so the user's own keys don't combine with it for long.
constexpr DWORD kShownTimeoutMs = 2000;  // building the menu takes well under a second, even with many windows
constexpr DWORD kShownForMs = 250;       // on screen briefly, like a quick press, before it's cancelled
constexpr DWORD kKeptTimeoutMs = 2000;   // the hooks check the menu right after it's dismissed
// Pressed before Alt is released: Alt pressed and released on its own would put the console in menu mode.
constexpr WORD kUnassignedKey = 0xE8;

// Opened for waiting only; null if the hooks running in that Explorer don't create it.
class HookEvent {
public:
    explicit HookEvent(const std::wstring& name) : handle_(OpenEventW(SYNCHRONIZE, FALSE, name.c_str())) {}
    ~HookEvent() {
        if (handle_) CloseHandle(handle_);
    }
    HookEvent(const HookEvent&) = delete;
    HookEvent& operator=(const HookEvent&) = delete;

    bool Exists() const { return handle_ != nullptr; }
    bool WaitSet(DWORD timeoutMs) const { return handle_ && WaitForSingleObject(handle_, timeoutMs) == WAIT_OBJECT_0; }

private:
    HANDLE handle_;
};

bool ConsoleInFront() {
    HWND console = GetConsoleWindow();
    HWND front = GetForegroundWindow();
    // In Windows Terminal the console window is a hidden one, owned by the terminal's window.
    return console && front && (front == console || GetAncestor(console, GA_ROOTOWNER) == front);
}

bool AnyKeyOrButtonHeld() {
    for (int key = VK_LBUTTON; key <= VK_OEM_CLEAR; ++key) {
        if (GetAsyncKeyState(key) & 0x8000) return true;
    }
    return false;
}

INPUT Key(WORD key, bool up) {
    INPUT input{.type = INPUT_KEYBOARD};
    input.ki.wVk = key;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    return input;
}

// Each batch goes in as one SendInput call, which the user's own keys can't interleave.
template <size_t count>
UINT Send(INPUT (&keys)[count]) {
    return SendInput(static_cast<UINT>(count), keys, sizeof(INPUT));
}

std::atomic<bool> g_altDown{false};

void ReleaseAlt() {
    if (!g_altDown.exchange(false)) return;
    INPUT keys[] = {Key(kUnassignedKey, false), Key(kUnassignedKey, true), Key(VK_MENU, true)};
    Send(keys);
}

// Closing the console window while Alt is down must not leave it down.
BOOL WINAPI ReleaseAltOnExit(DWORD) {
    ReleaseAlt();
    return FALSE;  // and let the process end as it would have
}

}  // namespace

std::optional<bool> IsKept(DWORD explorerPid) {
    HookEvent kept(SharedNames::MenuReadyEventName(explorerPid));
    if (!kept.Exists()) return std::nullopt;
    return kept.WaitSet(0);
}

bool CanOpenNow() { return ConsoleInFront() && !AnyKeyOrButtonHeld(); }

bool Open(DWORD explorerPid) {
    HookEvent opening(SharedNames::MenuOpeningEventName(explorerPid));
    HookEvent shown(SharedNames::MenuShownEventName(explorerPid));
    HookEvent kept(SharedNames::MenuReadyEventName(explorerPid));
    if (!opening.Exists() || !shown.Exists() || !kept.Exists()) return false;  // older hooks
    if (opening.WaitSet(0) || !ConsoleInFront()) return false;                 // a menu is open, or focus moved

    SetConsoleCtrlHandler(ReleaseAltOnExit, TRUE);
    // Alt stays down until the menu is on screen: released earlier, Windows gives up on building it.
    g_altDown = true;
    INPUT press[] = {Key(VK_MENU, false), Key(VK_TAB, false), Key(VK_TAB, true)};
    UINT pressed = Send(press);
    if (pressed == 2) {  // Tab went down but not up
        INPUT tabUp[] = {Key(VK_TAB, true)};
        Send(tabUp);
    }
    bool appeared = pressed >= 2 && shown.WaitSet(kShownTimeoutMs);
    if (appeared) Sleep(kShownForMs);
    // Esc only while a switcher has the keyboard: this opening, on screen or still being built (the
    // user may have closed it already), or another switcher that took the focus (classic Alt+Tab, a
    // replacement). With no switcher, Esc with Alt down is Alt+Esc, which sends the console to the
    // back; and releasing Alt alone while one is open would switch windows.
    bool switcherOpen = opening.WaitSet(0) || (pressed >= 2 && !appeared && !ConsoleInFront());
    if (switcherOpen) {
        INPUT cancel[] = {Key(VK_ESCAPE, false), Key(VK_ESCAPE, true)};
        Send(cancel);
    }
    ReleaseAlt();  // on every path: Alt must never stay down
    SetConsoleCtrlHandler(ReleaseAltOnExit, FALSE);
    return appeared && kept.WaitSet(kKeptTimeoutMs);
}

}  // namespace FirstAltTab
