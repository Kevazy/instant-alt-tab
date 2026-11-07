// Whether the kept menu still fits an opening: the checks before each reuse,
// the one-time inspection of its visual tree, and the display it was laid out
// for.

#include "KeptMenuState.h"

#include "QuietCardEdits.h"
#include "../shared/Log.h"

#include <shellscalingapi.h>

#include <cstring>

#include <winrt/Windows.UI.Xaml.Media.h>

namespace KeptMenu {
namespace {

using GetContentFn = HRESULT(__stdcall*)(IUnknown* window, IUnknown** content);

constexpr size_t kMaxTreeNodes = 8000;  // a menu of 125 cards has ~1,900 visual tree nodes
constexpr wchar_t kMenuWindowClass[] = L"XamlExplorerHostIslandWindow";

// The menu's top-level window: it has the foreground right after DisplayAltTab.
HWND MenuWindowInForeground() {
    HWND window = GetForegroundWindow();
    wchar_t className[64];
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    bool ours = pid == GetCurrentProcessId() && GetClassNameW(window, className, 64) &&
                std::wcscmp(className, kMenuWindowClass) == 0;
    return ours ? window : nullptr;
}

// The menu always opens on the display Windows passes to Show (on a typical
// setup the primary one, whichever monitor has the focused window), so the
// context is that display's monitor, read through the menu's own window.
// `display` is an object of the shell's, compared by address only (no
// reference is held); a display topology change also changes the monitor or
// work area read here.
std::optional<DisplayContext> ReadContext(void* display, HWND menuWindow) {
    DisplayContext c;
    c.display = display;
    HMONITOR monitor = menuWindow ? MonitorFromWindow(menuWindow, MONITOR_DEFAULTTOPRIMARY)
                                  : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    UINT dpiY = 0;
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (!GetMonitorInfoW(monitor, &info) || FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &c.dpi, &dpiY)) ||
        !SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)) {
        return std::nullopt;
    }
    c.monitor = info.szDevice;
    c.monitorBounds = info.rcMonitor;
    c.workArea = info.rcWork;
    c.highContrast = contrast.dwFlags & HCF_HIGHCONTRASTON;
    c.windowColor = GetSysColor(COLOR_WINDOW);
    constexpr wchar_t kPersonalize[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    DWORD size = sizeof(DWORD);
    RegGetValueW(HKEY_CURRENT_USER, kPersonalize, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &c.appsLightTheme,
                 &size);
    size = sizeof(DWORD);
    RegGetValueW(HKEY_CURRENT_USER, kPersonalize, L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr,
                 &c.systemLightTheme, &size);
    return c;
}

std::wstring ContextChanges(const DisplayContext& a, const DisplayContext& b) {
    std::wstring changes;
    if (a.display != b.display) changes += L" display";
    if (a.monitor != b.monitor || !EqualRect(&a.monitorBounds, &b.monitorBounds)) {
        changes += L" monitor (" + a.monitor + L" to " + b.monitor + L")";
    }
    if (!EqualRect(&a.workArea, &b.workArea)) changes += L" work-area";
    if (a.dpi != b.dpi) changes += L" dpi";
    if (a.appsLightTheme != b.appsLightTheme || a.systemLightTheme != b.systemLightTheme) changes += L" theme";
    if (a.highContrast != b.highContrast) changes += L" high-contrast";
    if (a.windowColor != b.windowColor) changes += L" colors";
    return changes;
}

}  // namespace

xaml::FrameworkElement AsElement(IUnknown* control) {
    winrt::Windows::Foundation::IInspectable value;
    winrt::copy_from_abi(value, control);
    return value.as<xaml::FrameworkElement>();
}

bool ListClosed(IUnknown* list) { return *(reinterpret_cast<BYTE*>(list) + g_listClosedOffset) != 0; }

// Whether the host window currently shows `control`, through its own content getter.
bool HostWindowShows(IUnknown* window, IUnknown* control) {
    if (!window || !control) return false;
    auto getter = VtableSlot(window, g_twinuiInternals.hostContentGetterSlot);
    auto setter = VtableSlot(window, g_twinuiInternals.hostContentSetterSlot);
    if (getter != g_twinuiImage.base + g_twinuiInternals.hostContentGetter ||
        setter != g_twinuiImage.base + g_twinuiInternals.hostContentSetter) {
        if (!g_failed) TurnOff(L"the host window's content accessors aren't where discovery found them");
        return false;
    }
    com_ptr<IUnknown> content;
    return SUCCEEDED(static_cast<GetContentFn>(getter)(window, content.put())) && SameObject(content.get(), control);
}

namespace {

// Finds, once per kept menu, what the hooks need from its visual tree.
// Returns false if the control doesn't show the card list it was kept with.
bool InspectTree(MenuState& kept) {
    std::vector<xaml::DependencyObject> queue{AsElement(kept.control.get())};
    bool showsList = false, elementTypeFound = false;
    for (size_t i = 0; i < queue.size() && i < kMaxTreeNodes; ++i) {
        xaml::DependencyObject node = queue[i];
        com_ptr<IUnknown> match;
        auto raw = static_cast<IUnknown*>(winrt::get_abi(node));
        if (SUCCEEDED(raw->QueryInterface(AltTab::kElementTypeNotInMenu, match.put_void()))) elementTypeFound = true;
        if (auto items = node.try_as<xaml::Controls::ItemsControl>()) {
            auto source = items.ItemsSource();
            if (source && SameObject(static_cast<IUnknown*>(winrt::get_abi(source)), kept.cards.get())) {
                showsList = true;
                if (auto panel = items.ItemsPanelRoot()) {
                    kept.itemsPanel.copy_from(static_cast<IUnknown*>(winrt::get_abi(panel)));
                }
            }
        }
        if (auto viewer = node.try_as<xaml::Controls::ScrollViewer>()) kept.scrollViewers.push_back(viewer);
        int children = xaml::Media::VisualTreeHelper::GetChildrenCount(node);
        for (int c = 0; c < children; ++c) queue.push_back(xaml::Media::VisualTreeHelper::GetChild(node, c));
    }
    kept.searchFindsNothing = queue.size() < kMaxTreeNodes && !elementTypeFound && kept.itemsPanel;
    return showsList;
}

}  // namespace

// Why the kept menu can't be shown for this opening, or nothing if it can.
std::optional<std::wstring> ReuseBlocker(void* host, void* display) {
    if (!g_kept) return L"first opening";
    MenuState& kept = *g_kept;
    // Alt was released while Show was building it, so Windows gave up on it before it went into its window.
    if (!kept.hidden && !kept.displayed) return L"previous menu dismissed before it appeared";
    if (!kept.hidden || !kept.hostWindow) return L"previous menu not kept";
    if (!kept.verified) return L"card list not verified";
    if (!g_cardItemOffset) return L"card layout not learned yet";
    if (kept.missedWindow) return L"card list missed a window";
    if (QuietCardEdits::CardVectorOf(kept.list.get()) != kept.cards.get()) return L"card list replaced";
    if (!SameObject(ComMember(host, g_twinuiInternals.hostDataSource), kept.dataSource.get()))
        return L"data source changed";
    if (!SameObject(ComMember(host, g_twinuiInternals.hostWindow), kept.hostWindow.get()) ||
        !HostWindowShows(kept.hostWindow.get(), kept.control.get())) {
        return L"host window changed";
    }
    if (ListClosed(kept.list.get())) return L"card list closed";
    if (kept.movedDisplay) return L"opened on another monitor";
    auto now = ReadContext(display, kept.window);
    if (!kept.context || !now) return L"display unreadable";
    std::wstring changes = ContextChanges(*kept.context, *now);
    if (changes.empty()) return std::nullopt;
    return L"display changed:" + changes;
}

// After a dismissal: verifies the kept menu once (its visual tree exists only
// after it was displayed), notes the display it's laid out for, and scrolls it
// back to the top, where a new menu always opens.
void TidyHiddenMenu() {
    MenuState& kept = *g_kept;
    if (!kept.displayed) return;
    try {
        if (!kept.inspected) {
            kept.inspected = true;
            kept.verified = InspectTree(kept);
            if (!kept.verified) {
                kept.list = nullptr;  // not this menu's: leave it to its owner
                TurnOff(L"the menu doesn't show the card list found when it was built");
                return;
            }
        }
        kept.context = ReadContext(kept.display, kept.window);
        kept.movedDisplay = false;
        for (auto& viewer : kept.scrollViewers) {
            if (viewer.VerticalOffset() > 0 || viewer.HorizontalOffset() > 0) {
                using Offset = winrt::Windows::Foundation::IReference<double>;
                viewer.ChangeView(Offset(0.0), Offset(0.0), nullptr, /*disableAnimation*/ true);
            }
        }
        SignalMenu(/*shown*/ false, kept.verified && g_cardItemOffset && !kept.missedWindow && kept.context);
    } catch (...) {
        TurnOff(L"tidying the hidden menu threw an exception");
    }
}

bool IsKeptCardContainer(IInspectable* root) {
    if (GetCurrentThreadId() != g_uiThread || !g_kept || !g_kept->searchFindsNothing) return false;
    try {
        xaml::DependencyObject node{nullptr};
        winrt::copy_from_abi(node, root);
        auto parent = xaml::Media::VisualTreeHelper::GetParent(node);
        return parent && SameObject(static_cast<IUnknown*>(winrt::get_abi(parent)), g_kept->itemsPanel.get());
    } catch (...) {
        return false;
    }
}

// The kept menu is on screen: a new one once it has loaded, a reused one at
// the end of Show.
void OnKeptMenuDisplayed() {
    MenuState& kept = *g_kept;
    kept.hidden = false;
    kept.displayed = true;
    SignalMenu(/*shown*/ true, /*ready*/ false);
    if (HWND window = MenuWindowInForeground()) kept.window = window;
    // Windows chose another monitor than the one the menu was laid out for.
    if (kept.context && kept.window) {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        HMONITOR now = MonitorFromWindow(kept.window, MONITOR_DEFAULTTONULL);
        if (!now || !GetMonitorInfoW(now, &info) || kept.context->monitor != info.szDevice) kept.movedDisplay = true;
    }
}

bool ShowsKeptMenu(void* host) {
    return g_kept && SameObject(ComMember(host, g_twinuiInternals.hostControl), g_kept->control.get());
}

}  // namespace KeptMenu
