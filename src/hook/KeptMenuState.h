#pragma once

// What the three files that keep the menu share: the state of the kept menu
// and of the opening in progress, and what each file offers the others.
// KeptMenu.cpp has the hooks and the menu's life (keeping, dropping,
// turning off); KeptMenuChecks.cpp decides whether the kept menu still fits
// an opening; MenuSelection.cpp sets the selection each opening starts with.

#include "ComObjects.h"
#include "Hooking.h"

#include <inspectable.h>

#include <atomic>
#include <optional>
#include <string>
#include <vector>

#undef GetCurrentTime  // winbase.h macro that collides with a C++/WinRT member
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.h>

namespace KeptMenu {

namespace xaml = winrt::Windows::UI::Xaml;
using winrt::com_ptr;

// What a kept menu was laid out for. Any difference rebuilds it.
struct DisplayContext {
    void* display = nullptr;  // Show's display argument, compared by identity
    HMONITOR monitor = nullptr;
    RECT workArea{};
    UINT dpi = 0;
    DWORD appsLightTheme = 1, systemLightTheme = 1;
    bool highContrast = false;
    COLORREF windowColor = 0;
};

struct MenuState {
    com_ptr<IUnknown> control;              // the AltTab XAML control
    com_ptr<IUnknown> dataSource;           // what it was built from
    com_ptr<IUnknown> list;                 // its card list view model
    com_ptr<IUnknown> cards;                // the list's card vector, held so its address can't be reused
    com_ptr<IUnknown> hostWindow;           // keeps the control attached while hidden; known after the first dismissal
    void* display = nullptr;                // Show's display argument when it was built
    HWND window = nullptr;                  // the menu's top-level window, found when first displayed
    std::optional<DisplayContext> context;  // read while hidden, after each dismissal
    bool displayed = false;
    bool hidden = false;
    bool movedDisplay = false;  // the last opening showed up on another monitor than the context says
    bool closeSkipped = false;  // a card was chosen and the list was kept subscribed
    bool missedWindow = false;  // Show navigated to a window none of its cards holds

    // Found in the visual tree at the first dismissal after it was displayed.
    bool inspected = false;
    bool verified = false;  // the control shows `list`'s card vector
    com_ptr<IUnknown> itemsPanel;
    std::vector<xaml::Controls::ScrollViewer> scrollViewers;
    bool searchFindsNothing = false;

    // Show registers the XAML root on every opening; a kept menu reuses its registration.
    com_ptr<IUnknown> registrar, root, registration;
};

// How Show first moved the selection; see NavigateToItemHook.
enum class FirstNavigation { None, ToItem, Forward, Backward };

// The Show in progress.
struct Opening {
    void* host;
    void* display;
    int flags;
    bool reused = false;
    bool displayDeferred = false;
    bool cancelled = false;   // dismissed before Show returned
    com_ptr<IUnknown> built;  // the control CreateAltTabControl built in this opening
    bool confirmed = false;   // ConfirmBuiltControl ran for it
    FirstNavigation firstNavigation = FirstNavigation::None;
};

// The state, defined in KeptMenu.cpp unless noted. Touched only on
// Explorer's UI thread (the thread that first shows the menu), except the
// atomics.
extern ModuleImage g_twinuiImage, g_switcherImage;
extern AltTab::TwinuiInternals g_twinuiInternals;  // what discovery found in twinui
extern uint32_t g_listClosedOffset;                // and the card list member closeList sets
extern std::optional<MenuState> g_kept;
extern Opening* g_opening;
extern std::atomic<bool> g_enabled;
extern std::atomic<bool> g_failed;  // turned off because something unexpected happened
extern std::atomic<DWORD> g_uiThread;
extern SRWLOCK g_uiThreadLock;  // claiming the UI thread vs. stopping
extern bool g_stopping;
extern size_t g_cardItemOffset;  // where a card keeps its ISwitchItem (MenuSelection.cpp)

// KeptMenu.cpp
// Fails closed: something unexpected happened, so stop keeping menus.
void TurnOff(const wchar_t* reason);

// KeptMenuChecks.cpp
xaml::FrameworkElement AsElement(IUnknown* control);
bool ListClosed(IUnknown* list);
// Whether the host window currently shows `control`, through its own content getter.
bool HostWindowShows(IUnknown* window, IUnknown* control);
// Why the kept menu can't be shown for this opening, or nothing if it can.
std::optional<std::wstring> ReuseBlocker(void* host, void* display);
// After a dismissal: verifies the kept menu once (its visual tree exists only
// after it was displayed), notes the display it's laid out for, and scrolls it
// back to the top, where a new menu always opens.
void TidyHiddenMenu();
// Whether `root` is one of the kept menu's card containers, known to hold none
// of the element type the switcher searches for.
bool IsKeptCardContainer(IInspectable* root);
// The kept menu is on screen: a new one once it has loaded, a reused one at
// the end of Show.
void OnKeptMenuDisplayed();
bool ShowsKeptMenu(void* host);

// MenuSelection.cpp
// Hooks the navigation once the host holds the built control itself (not
// another interface of it), or refuses the build.
void ConfirmBuiltControl(Opening& opening);
// Moves the selection to the first card through the control's own NavigateToItem.
void SelectFirstCard(IUnknown* control);
// Leaves the kept menu's selected card with keyboard focus, as a new menu has it.
void ShowSelectionAsKeyboardFocus();

}  // namespace KeptMenu
