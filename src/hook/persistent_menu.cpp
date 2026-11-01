#include "persistent_menu.h"

#include "card_list.h"
#include "../shared/log.h"
#include "../shared/protocol.h"

#include <intrin.h>
#include <shellscalingapi.h>

#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <vector>

#undef GetCurrentTime  // winbase.h macro that collides with a C++/WinRT member
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace persistent_menu {
namespace {

namespace xaml = winrt::Windows::UI::Xaml;
using winrt::com_ptr;

// clang-format reads some of these parameter lists as multiplications.
// clang-format off
using CreateControlFn = IUnknown*(__fastcall*)(void* host);
using ShowFn = HRESULT(__fastcall*)(void* host, void* display, int flags, void* view);
using HostFn = void(__fastcall*)(void* host);
using SetHostContentFn = void(__fastcall*)(IUnknown** window, IUnknown** content);
using RegisterRootFn = IUnknown*(__fastcall*)(IUnknown* registrar, IUnknown* root);
using CloseListFn = void(__fastcall*)(void* list);
using GetItemForViewFn = void*(__fastcall*)(void* source, void* view, void*, void*);
using FindDescendantFn = IInspectable*(__fastcall*)(IInspectable* root);
using GetContentFn = HRESULT(__stdcall*)(IUnknown* window, IUnknown** content);
using NavigateToItemFn = HRESULT(__fastcall*)(IUnknown* control, IUnknown* item);
using NavigateFn = HRESULT(__fastcall*)(IUnknown* control, bool backward);
// clang-format on

CreateControlFn g_createControl;
ShowFn g_show;
HostFn g_dismiss, g_display;
SetHostContentFn g_setHostContent;
RegisterRootFn g_registerRoot;
CloseListFn g_closeList;
GetItemForViewFn g_getItemForView;
FindDescendantFn g_findDescendant;
NavigateToItemFn g_navigateToItem;
NavigateFn g_navigate;
ModuleImage g_twinui, g_switcher;
internals::Twinui g_found;  // what discovery found in twinui
uint32_t g_listClosed = 0;  // and the card list member closeList sets

constexpr size_t kMaxTreeNodes = 8000;    // a menu of 125 cards has ~1,900 visual tree nodes
constexpr size_t kCardScanBytes = 0x200;  // how far into a card its item is looked for
constexpr wchar_t kMenuWindowClass[] = L"XamlExplorerHostIslandWindow";

// ---------------------------------------------------------------------------
// State. Everything below is touched only on Explorer's UI thread (the thread
// that first shows the menu), except the flags marked atomic.

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

struct KeptMenu {
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
    bool noSearchedType = false;

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

std::optional<KeptMenu> g_kept;
Opening* g_opening = nullptr;
void* g_dismissingHost = nullptr;
void* g_displayedHost = nullptr;             // displayed by ShowHook; another DisplayAltTab for it is a no-op
size_t g_cardItemOffset = 0;                 // where a card keeps its ISwitchItem; learned from Show's NavigateToItem
std::array<void*, 2> g_navigationTargets{};  // the control's NavigateToItem and Navigate, once hooked
bool g_closeListSeen = false;                // Windows was seen calling closeList with a card list
thread_local bool t_selectingFirstCard = false;
std::vector<com_ptr<IUnknown>> g_listsToClose;

std::atomic<bool> g_enabled{false};
std::atomic<HANDLE> g_activeMarker{nullptr};  // exists while g_enabled; see protocol::ActiveMarkerName
std::atomic<DWORD> g_uiThread{0};
SRWLOCK g_uiThreadLock = SRWLOCK_INIT;  // claiming the UI thread vs. stopping
bool g_stopping = false;
std::atomic<HWND> g_worker{nullptr};  // message-only window on the UI thread for deferred work
wchar_t g_workerClass[64];
std::atomic<bool> g_turnOffRequested{false}, g_finalStop{false}, g_failed{false};
HANDLE g_stoppedEvent = nullptr;
HANDLE g_turnedOffEvent = nullptr;  // see protocol::TurnedOffEventName

// ---------------------------------------------------------------------------
// Helpers

IUnknown*& Member(void* object, size_t offset) {
    return *reinterpret_cast<IUnknown**>(static_cast<BYTE*>(object) + offset);
}

void* VtableSlot(IUnknown* object, size_t slot) { return (*reinterpret_cast<void***>(object))[slot]; }

bool SameObject(IUnknown* a, IUnknown* b) {
    if (!a || !b) return a == b;
    com_ptr<IUnknown> x, y;
    return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(x.put()))) && SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(y.put()))) &&
           x == y;
}

// A pointer that looks like a COM object: its vtable and the vtable's first
// entry (QueryInterface) are in loaded modules. Checked before calling into a
// member read at a discovered offset.
bool LooksLikeComObject(const void* object) {
    auto inModule = [](const void* address) {
        HMODULE module;
        return address &&
               GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCWSTR>(address), &module);
    };
    void* vtable = object ? ReadPointer(object, 0) : nullptr;
    return inModule(vtable) && inModule(ReadPointer(vtable, 0));
}

// A COM pointer member of the host, or null if what's there isn't one.
IUnknown* ComMember(void* host, size_t offset) {
    auto value = static_cast<IUnknown*>(ReadPointer(host, offset));
    return LooksLikeComObject(value) ? value : nullptr;
}

bool ListClosed(IUnknown* list) { return *(reinterpret_cast<BYTE*>(list) + g_listClosed) != 0; }

void TurnOff(const wchar_t* reason);

// Whether the host window currently shows `control`, through its own content getter.
bool HostWindowShows(IUnknown* window, IUnknown* control) {
    if (!window || !control) return false;
    auto getter = VtableSlot(window, g_found.hostContentGetterSlot);
    auto setter = VtableSlot(window, g_found.hostContentSetterSlot);
    if (getter != g_twinui.base + g_found.hostContentGetter || setter != g_twinui.base + g_found.hostContentSetter) {
        if (!g_failed) TurnOff(L"the host window's content accessors aren't where discovery found them");
        return false;
    }
    com_ptr<IUnknown> content;
    return SUCCEEDED(static_cast<GetContentFn>(getter)(window, content.put())) && SameObject(content.get(), control);
}

xaml::FrameworkElement AsElement(IUnknown* control) {
    winrt::Windows::Foundation::IInspectable value;
    winrt::copy_from_abi(value, control);
    return value.as<xaml::FrameworkElement>();
}

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
    c.monitor = menuWindow ? MonitorFromWindow(menuWindow, MONITOR_DEFAULTTOPRIMARY)
                           : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    UINT dpiY = 0;
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (!GetMonitorInfoW(c.monitor, &info) || FAILED(GetDpiForMonitor(c.monitor, MDT_EFFECTIVE_DPI, &c.dpi, &dpiY)) ||
        !SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)) {
        return std::nullopt;
    }
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
    if (a.monitor != b.monitor) changes += L" monitor";
    if (!EqualRect(&a.workArea, &b.workArea)) changes += L" work-area";
    if (a.dpi != b.dpi) changes += L" dpi";
    if (a.appsLightTheme != b.appsLightTheme || a.systemLightTheme != b.systemLightTheme) changes += L" theme";
    if (a.highContrast != b.highContrast) changes += L" high-contrast";
    if (a.windowColor != b.windowColor) changes += L" colors";
    return changes;
}

com_ptr<IUnknown> SwitchItemOf(IInspectable* card) {
    com_ptr<IUnknown> item;
    auto field = g_cardItemOffset ? static_cast<IUnknown*>(ReadPointer(card, g_cardItemOffset)) : nullptr;
    if (LooksLikeComObject(field)) field->QueryInterface(g_found.switchItem, item.put_void());
    return item;
}

// How many cards hold `item` where cards keep their item.
size_t CardsHolding(IUnknown* item) {
    size_t holders = 0;
    for (IInspectable* card : card_list::Cards(g_kept->cards.get())) {
        holders += ReadPointer(card, g_cardItemOffset) == item;
    }
    return holders;
}

// Learns where a card keeps its item from the item Show selects: exactly one
// card may hold that pointer, at exactly one offset.
void LearnCardItemOffset(IUnknown* item) {
    com_ptr<IUnknown> switchItem;
    item->QueryInterface(g_found.switchItem, switchItem.put_void());
    if (switchItem.get() != item) {
        Log(L"Show's item isn't a card's ISwitchItem; menus aren't kept");
        return;
    }
    std::set<size_t> offsets;
    size_t holders = 0;
    for (IInspectable* card : card_list::Cards(g_kept->cards.get())) {
        bool holds = false;
        for (size_t offset = sizeof(void*); offset < kCardScanBytes; offset += sizeof(void*)) {
            if (ReadPointer(card, offset) == item) {
                offsets.insert(offset);
                holds = true;
            }
        }
        holders += holds;
    }
    if (holders == 1 && offsets.size() == 1) {
        g_cardItemOffset = *offsets.begin();
        Log(L"cards keep their item at +0x%zx", g_cardItemOffset);
    } else {
        Log(L"couldn't tell where cards keep their item (%zu cards hold it, at %zu offsets)", holders, offsets.size());
    }
}

// Moves the selection to the first card through the control's own NavigateToItem.
void SelectFirstCard(IUnknown* control) {
    auto cards = card_list::Cards(g_kept->cards.get());
    if (cards.empty() || !control) return;
    com_ptr<IUnknown> item = SwitchItemOf(cards.front());
    if (!item) return;
    auto navigate = static_cast<NavigateToItemFn>(VtableSlot(control, g_found.navigateToItemSlot));
    t_selectingFirstCard = true;
    HRESULT hr = navigate(control, item.get());
    t_selectingFirstCard = false;
    if (FAILED(hr)) Log(L"selecting the first card failed (0x%08lx)", hr);
}

// A new menu shows its selection with keyboard focus. The control's navigation
// focuses cards "programmatically", which XAML turns into the input mode the
// control saw last: after a kept menu was dismissed with the mouse that is
// Pointer, and the selection border isn't drawn. So every navigation of the
// kept menu leaves the selected card with keyboard focus.
void ShowSelectionAsKeyboardFocus() {
    try {
        auto root = AsElement(g_kept->control.get()).XamlRoot();
        if (!root) return;  // a new control isn't in a XAML island yet
        auto focused = xaml::Input::FocusManager::GetFocusedElement(root).try_as<xaml::Controls::Control>();
        if (focused && focused.FocusState() != xaml::FocusState::Keyboard) focused.Focus(xaml::FocusState::Keyboard);
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Keeping and dropping the menu

// A list that a kept menu kept subscribed is closed the way the switcher
// closes it after a card is chosen, so the data source releases it. Deferred
// to the worker window: dropping can happen inside Show.
void CloseQueuedLists() {
    auto lists = std::move(g_listsToClose);
    g_listsToClose.clear();
    for (auto& list : lists) {
        if (ListClosed(list.get())) continue;
        // Called directly only once Windows itself was seen calling it with a card list.
        if (!g_closeListSeen) {
            Log(L"left a dropped card list open: its close function hasn't been seen in use yet");
            continue;
        }
        try {
            g_closeList(list.get());
        } catch (...) {
            Log(L"closing a dropped menu's card list failed");
        }
    }
}

void DropKeptMenu() {
    if (!g_kept) return;
    KeptMenu& kept = *g_kept;
    card_list::SetQuietVector(nullptr);
    // Only a list known to be this menu's, or one kept subscribed after a card
    // was chosen, is closed: anything else belongs to whoever created it.
    if (kept.list && !ListClosed(kept.list.get()) && (kept.verified || kept.closeSkipped)) {
        g_listsToClose.push_back(kept.list);
        if (HWND worker = g_worker) PostMessageW(worker, WM_APP, 0, 0);
    }
    if (kept.hidden && HostWindowShows(kept.hostWindow.get(), kept.control.get())) {
        IUnknown* window = kept.hostWindow.get();
        IUnknown* empty = nullptr;
        try {
            g_setHostContent(&window, &empty);
        } catch (...) {
        }
        if (HostWindowShows(kept.hostWindow.get(), kept.control.get())) Log(L"detaching the hidden menu failed");
    }
    g_kept.reset();
}

void Disable() {
    g_enabled = false;
    if (HANDLE marker = g_activeMarker.exchange(nullptr)) CloseHandle(marker);
}

// Runs from the worker window, never during an opening (a drop inside Show is
// followed by the new menu taking the focus anyway).
void TurnOffNow() {
    Disable();
    if (g_failed && g_turnedOffEvent) SetEvent(g_turnedOffEvent);
    HWND focused = GetForegroundWindow();
    DropKeptMenu();
    // Detaching the hidden menu can hand the keyboard focus to its invisible window.
    HWND now = GetForegroundWindow();
    if (now != focused && focused && IsWindow(focused) && now && !IsWindowVisible(now)) SetForegroundWindow(focused);
}

// Fails closed: something unexpected happened, so stop keeping menus.
void TurnOff(const wchar_t* reason) {
    Log(L"turning off: %s", reason);
    g_failed = true;
    g_turnOffRequested = true;
    if (HWND worker = g_worker) PostMessageW(worker, WM_APP, 0, 0);
}

// Deferred work runs on the worker window, never nested inside a hooked call.
LRESULT CALLBACK WorkerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    constexpr UINT_PTR kRetryTimer = 1;
    if (message != WM_APP && !(message == WM_TIMER && wParam == kRetryTimer)) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    if (HookScope::InsideHookOnThisThread()) {
        SetTimer(window, kRetryTimer, 50, nullptr);
        return 0;
    }
    KillTimer(window, kRetryTimer);
    if (g_turnOffRequested.exchange(false)) TurnOffNow();
    CloseQueuedLists();
    if (g_finalStop) {
        g_worker = nullptr;
        DestroyWindow(window);
        UnregisterClassW(g_workerClass, reinterpret_cast<HINSTANCE>(&__ImageBase));
        SetEvent(g_stoppedEvent);
    }
    return 0;
}

// The first thread that shows the menu owns the kept menu and the worker window.
bool OnUiThread() {
    DWORD thread = GetCurrentThreadId();
    if (DWORD owner = g_uiThread) return owner == thread;
    AcquireSRWLockExclusive(&g_uiThreadLock);
    if (!g_uiThread && !g_stopping) {
        swprintf_s(g_workerClass, L"InstantAltTabWorker-%p", static_cast<void*>(&__ImageBase));
        WNDCLASSW windowClass{.lpfnWndProc = WorkerProc,
                              .hInstance = reinterpret_cast<HINSTANCE>(&__ImageBase),
                              .lpszClassName = g_workerClass};
        RegisterClassW(&windowClass);
        g_worker = CreateWindowExW(0, g_workerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance,
                                   nullptr);
        if (g_worker) g_uiThread = thread;  // without a worker nothing could be cleaned up, so nothing is kept
    }
    bool owns = g_uiThread == thread;
    ReleaseSRWLockExclusive(&g_uiThreadLock);
    return owns;
}

// Why the kept menu can't be shown for this opening, or nothing if it can.
std::optional<std::wstring> ReuseBlocker(void* host, void* display) {
    if (!g_kept) return L"first opening";
    KeptMenu& kept = *g_kept;
    if (!kept.hidden || !kept.hostWindow) return L"previous menu not kept";
    if (!kept.verified) return L"card list not verified";
    if (!g_cardItemOffset) return L"card layout not learned yet";
    if (kept.missedWindow) return L"card list missed a window";
    if (card_list::CardVectorOf(kept.list.get()) != kept.cards.get()) return L"card list replaced";
    if (!SameObject(ComMember(host, g_found.hostDataSource), kept.dataSource.get())) return L"data source changed";
    if (!SameObject(ComMember(host, g_found.hostWindow), kept.hostWindow.get()) ||
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

HRESULT __fastcall NavigateToItemHook(IUnknown* control, IUnknown* item);
HRESULT __fastcall NavigateHook(IUnknown* control, bool backward);

// The control's NavigateToItem and Navigate are read from its own vtable, at
// the slots twinui calls them through. That is only right for the interface
// pointer twinui calls them on, so they're hooked once the host is seen
// holding exactly the pointer the factory returned, in the first opening that
// builds a menu. Later controls must have the same ones.
bool HookNavigation(IUnknown* control) {
    std::array<void*, 2> targets{VtableSlot(control, g_found.navigateToItemSlot),
                                 VtableSlot(control, g_found.navigateSlot)};
    if (g_navigationTargets[0]) return targets == g_navigationTargets;
    if (!IsCode(g_switcher, targets[0]) || !IsCode(g_switcher, targets[1]) || targets[0] == targets[1]) return false;
    // A Stop on another thread disables all hooks; none may be enabled after it.
    AcquireSRWLockExclusive(&g_uiThreadLock);
    bool hooked = !g_stopping && HookNow(targets[0], &NavigateToItemHook, &g_navigateToItem);
    hooked = hooked && HookNow(targets[1], &NavigateHook, &g_navigate);
    ReleaseSRWLockExclusive(&g_uiThreadLock);
    if (hooked) g_navigationTargets = targets;
    return hooked;
}

// Hooks the navigation once the host holds the built control itself (not
// another interface of it), or refuses the build.
void ConfirmBuiltControl(Opening& opening) {
    if (opening.confirmed || !opening.built || opening.cancelled || !g_kept || g_kept->control != opening.built) return;
    opening.confirmed = true;
    if (ReadPointer(opening.host, g_found.hostControl) != opening.built.get()) {
        TurnOff(L"the host doesn't hold its control where discovery found it");
    } else if (!HookNavigation(opening.built.get())) {
        TurnOff(L"the control's navigation isn't the one hooked before");
    }
}

void Keep(IUnknown* control, void* host, void* list, void* display) {
    void* cards = card_list::CardVectorOf(list);
    IUnknown* dataSource = ComMember(host, g_found.hostDataSource);
    if (!cards || !dataSource) {
        Log(L"menu not kept: its card list or data source isn't the expected one");
        return;
    }
    com_ptr<IUnknown> typed;
    dataSource->QueryInterface(g_found.switchItemDataSource, typed.put_void());
    if (!typed) {
        TurnOff(L"the host's data source member isn't a data source");
        return;
    }
    KeptMenu& kept = g_kept.emplace();
    kept.display = display;
    kept.control.copy_from(control);
    kept.dataSource.copy_from(dataSource);
    kept.list.copy_from(static_cast<IUnknown*>(list));
    kept.cards.copy_from(static_cast<IUnknown*>(cards));
    card_list::SetQuietVector(cards);
}

// Finds, once per kept menu, what the hooks need from its visual tree.
// Returns false if the control doesn't show the card list it was kept with.
bool InspectTree(KeptMenu& kept) {
    std::vector<xaml::DependencyObject> queue{AsElement(kept.control.get())};
    bool showsList = false, searchedTypeFound = false;
    for (size_t i = 0; i < queue.size() && i < kMaxTreeNodes; ++i) {
        xaml::DependencyObject node = queue[i];
        com_ptr<IUnknown> match;
        auto raw = static_cast<IUnknown*>(winrt::get_abi(node));
        if (SUCCEEDED(raw->QueryInterface(internals::kSearchedType, match.put_void()))) searchedTypeFound = true;
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
    kept.noSearchedType = queue.size() < kMaxTreeNodes && !searchedTypeFound && kept.itemsPanel;
    return showsList;
}

// After a dismissal: verifies the kept menu once (its visual tree exists only
// after it was displayed), notes the display it's laid out for, and scrolls it
// back to the top, where a new menu always opens.
void TidyHiddenMenu() {
    KeptMenu& kept = *g_kept;
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
    } catch (...) {
        TurnOff(L"tidying the hidden menu threw an exception");
    }
}

bool IsKeptCardContainer(IInspectable* root) {
    if (GetCurrentThreadId() != g_uiThread || !g_kept || !g_kept->noSearchedType) return false;
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
    KeptMenu& kept = *g_kept;
    kept.hidden = false;
    kept.displayed = true;
    if (HWND window = MenuWindowInForeground()) kept.window = window;
    // Windows chose another monitor than the one the menu was laid out for.
    if (kept.context && kept.window && MonitorFromWindow(kept.window, MONITOR_DEFAULTTONULL) != kept.context->monitor) {
        kept.movedDisplay = true;
    }
}

bool ShowsKeptMenu(void* host) {
    return g_kept && SameObject(ComMember(host, g_found.hostControl), g_kept->control.get());
}

// ---------------------------------------------------------------------------
// Hooks

HRESULT __fastcall ShowHook(void* host, void* display, int flags, void* view) {
    HookScope scope;
    if (!g_enabled || g_opening || !OnUiThread()) return g_show(host, display, flags, view);

    Opening opening{host, display, flags};
    g_opening = &opening;
    g_displayedHost = nullptr;
    HRESULT hr;
    try {
        hr = g_show(host, display, flags, view);
    } catch (...) {
        g_opening = nullptr;
        TurnOff(L"Show threw an exception");
        throw;
    }
    g_opening = nullptr;
    if (FAILED(hr)) {
        TurnOff(L"Show failed");
        return hr;
    }
    // Normally confirmed in GetItemForViewHook already; this covers a Show that doesn't call it.
    ConfirmBuiltControl(opening);
    // Show moved forward from the kept selection: start where a new menu starts instead.
    if (opening.reused && !opening.cancelled && opening.firstNavigation == FirstNavigation::Forward) {
        SelectFirstCard(ComMember(host, g_found.hostControl));
    }

    // Display only now, after laying out the menu, so its first frame is complete.
    if (opening.cancelled || !(opening.reused || opening.displayDeferred) || !ComMember(host, g_found.hostWindow)) {
        return hr;
    }
    try {
        AsElement(ComMember(host, g_found.hostControl)).UpdateLayout();
    } catch (...) {
        TurnOff(L"layout failed");
    }
    g_displayedHost = host;
    g_display(host);
    if (ShowsKeptMenu(host)) {
        OnKeptMenuDisplayed();
        ShowSelectionAsKeyboardFocus();  // displaying restores focus in the input mode last seen
    }
    return hr;
}

IUnknown* __fastcall CreateControlHook(void* host) {
    HookScope scope;
    if (!g_enabled || GetCurrentThreadId() != g_uiThread || !g_opening || g_opening->host != host) {
        return g_createControl(host);
    }
    Opening* opening = g_opening;
    auto blocker = ReuseBlocker(host, opening->display);
    if (!blocker) {
        opening->reused = true;
        g_kept->control->AddRef();
        return g_kept->control.get();
    }
    DropKeptMenu();
    unsigned sorts = card_list::SortCount();
    IUnknown* control = g_createControl(host);
    opening->built.copy_from(control);
    // Building the control creates its card list and sorts it once.
    if (control && card_list::SortCount() != sorts) Keep(control, host, card_list::LastSortedList(), opening->display);
    Log(L"menu built (%s)", blocker->c_str());
    return control;
}

void __fastcall DismissHook(void* host) {
    HookScope scope;
    if (!g_enabled || GetCurrentThreadId() != g_uiThread) return g_dismiss(host);
    if (g_opening && g_opening->host == host) g_opening->cancelled = true;
    void* previous = g_dismissingHost;
    g_dismissingHost = host;
    try {
        g_dismiss(host);
    } catch (...) {
        g_dismissingHost = previous;
        TurnOff(L"dismiss threw an exception");
        throw;
    }
    g_dismissingHost = previous;
    if (g_displayedHost == host) g_displayedHost = nullptr;
    if (g_kept && g_kept->hidden) TidyHiddenMenu();
}

void __fastcall SetHostContentHook(IUnknown** window, IUnknown** content) {
    HookScope scope;
    // A newly built menu must go into the host window member discovery found.
    Opening* opening = g_opening;
    if (g_enabled && opening && opening->built && *content && GetCurrentThreadId() == g_uiThread &&
        window != &Member(opening->host, g_found.hostWindow) && SameObject(*content, opening->built.get())) {
        TurnOff(L"the host doesn't hold its window where discovery found it");
    }
    if (g_enabled && g_kept && GetCurrentThreadId() == g_uiThread) {
        KeptMenu& kept = *g_kept;
        // Dismissal clears the content: keep the menu attached, hidden.
        void* host = g_dismissingHost;
        if (host && window == &Member(host, g_found.hostWindow) && !*content &&
            SameObject(ComMember(host, g_found.hostControl), kept.control.get()) &&
            HostWindowShows(*window, kept.control.get())) {
            kept.hostWindow.copy_from(*window);
            kept.hidden = true;
            return;
        }
        // A reused opening sets the same control again: nothing to do.
        if (g_opening && g_opening->reused && window == &Member(g_opening->host, g_found.hostWindow) && *content &&
            SameObject(*content, kept.control.get()) && HostWindowShows(*window, kept.control.get())) {
            return;
        }
    }
    g_setHostContent(window, content);
}

void __fastcall DisplayHook(void* host) {
    HookScope scope;
    if (!g_enabled || GetCurrentThreadId() != g_uiThread) return g_display(host);
    if (g_opening && g_opening->host == host) {
        g_opening->displayDeferred = true;  // displayed after layout, at the end of ShowHook
        return;
    }
    if (g_displayedHost == host) return;
    g_display(host);
    if (ShowsKeptMenu(host)) OnKeptMenuDisplayed();
}

IUnknown* __fastcall RegisterRootHook(IUnknown* registrar, IUnknown* root) {
    HookScope scope;
    bool fromShow = _ReturnAddress() == g_twinui.base + g_found.registerRootReturnInShow;
    if (!fromShow || !g_enabled || GetCurrentThreadId() != g_uiThread) return g_registerRoot(registrar, root);
    Opening* opening = g_opening;
    if (!opening || !g_kept || !SameObject(ComMember(opening->host, g_found.hostControl), g_kept->control.get())) {
        return g_registerRoot(registrar, root);
    }
    KeptMenu& kept = *g_kept;
    if (kept.registration && SameObject(root, kept.root.get()) && SameObject(registrar, kept.registrar.get())) {
        kept.registration->AddRef();
        return kept.registration.get();
    }
    IUnknown* registration = g_registerRoot(registrar, root);
    if (registration) {
        kept.registration.copy_from(registration);
        kept.root.copy_from(root);
        kept.registrar.copy_from(registrar);
    }
    return registration;
}

// Show calls this after storing the control it built and before positioning
// the selection: the moment to confirm the control and hook its navigation.
void* __fastcall GetItemForViewHook(void* source, void* view, void* a, void* b) {
    HookScope scope;
    Opening* opening = g_opening;
    if (g_enabled && opening && GetCurrentThreadId() == g_uiThread) ConfirmBuiltControl(*opening);
    return g_getItemForView(source, view, a, b);
}

// The switcher closes a list when a card is chosen: it stops listening to the
// data source because the menu is about to be thrown away. The kept menu's list
// must keep listening, or it misses every window change from then on.
void __fastcall CloseListHook(void* list) {
    HookScope scope;
    if (card_list::CardVectorOf(list)) g_closeListSeen = true;
    if (g_enabled && g_kept && g_kept->list.get() == list && GetCurrentThreadId() == g_uiThread) {
        g_kept->closeSkipped = true;
        return;
    }
    g_closeList(list);
}

bool IsKeptControl(IUnknown* control) {
    return g_enabled && GetCurrentThreadId() == g_uiThread && g_kept && g_kept->control.get() == control;
}

// Show sets the starting selection on the control: with the foreground
// window's item it calls NavigateToItem(item) and then moves forward (the
// second card); for Alt+Shift+Tab it only moves backward; when the foreground
// window has no card (the taskbar, the desktop) it only moves forward. The
// last two move from the control's current position, which in a new control
// is "none" (forward lands on the first card, backward on the last). A kept
// control still has the position the previous opening ended on, so it is
// reset: to the first card before a backward first move (which wraps to the
// last card), and to the first card after Show for a forward first move (see
// ShowHook). The direction comes from the calls themselves; Tab presses after
// Show (no opening in progress) are left alone.
HRESULT __fastcall NavigateToItemHook(IUnknown* control, IUnknown* item) {
    HookScope scope;
    Opening* opening = g_opening;
    if (opening && LooksLikeComObject(item) && !t_selectingFirstCard && IsKeptControl(control)) {
        if (!g_cardItemOffset) {
            LearnCardItemOffset(item);
        } else if (size_t holders = CardsHolding(item); holders > 1) {
            TurnOff(L"cards no longer hold their item where they did");
        } else if (holders == 0) {
            g_kept->missedWindow = true;  // a window too new for the kept list: rebuild next time
            Log(L"the kept card list doesn't have the foreground window yet");
        }
        if (opening->firstNavigation == FirstNavigation::None) opening->firstNavigation = FirstNavigation::ToItem;
    }
    HRESULT hr = g_navigateToItem(control, item);
    if (IsKeptControl(control)) ShowSelectionAsKeyboardFocus();
    return hr;
}

HRESULT __fastcall NavigateHook(IUnknown* control, bool backward) {
    HookScope scope;
    Opening* opening = g_opening;
    if (opening && IsKeptControl(control) && opening->firstNavigation == FirstNavigation::None) {
        opening->firstNavigation = backward ? FirstNavigation::Backward : FirstNavigation::Forward;
        if (backward && opening->reused) SelectFirstCard(control);
    }
    HRESULT hr = g_navigate(control, backward);
    if (IsKeptControl(control)) ShowSelectionAsKeyboardFocus();
    return hr;
}

// After each card change the switcher searches every card's subtree for an
// element type the Alt+Tab menu never contains (~470 searches, ~15 ms per
// opening with 125 cards). For the kept menu's cards, checked once to contain
// no such element, the answer is known.
IInspectable* __fastcall FindDescendantHook(IInspectable* root) {
    HookScope scope;
    static thread_local int depth = 0;  // the search is recursive; only the outermost call is answered
    if (depth == 0 && g_enabled && IsKeptCardContainer(root)) return nullptr;
    ++depth;
    IInspectable* found;
    try {
        found = g_findDescendant(root);
    } catch (...) {
        --depth;
        throw;
    }
    --depth;
    return found;
}

}  // namespace

bool Install(const ModuleImage& twinui, const ModuleImage& switcher, const internals::Internals& found) {
    g_twinui = twinui;
    g_switcher = switcher;
    g_found = found.twinui;
    g_listClosed = found.switcher.listClosed;
    // The one real RegisterXamlRoot call: a direct call to it right before that return address.
    const BYTE* call = twinui.base + g_found.registerRootReturnInShow - 5;
    if (!IsCode(twinui, call) || !IsCode(twinui, call + 4) || call[0] != 0xE8) return false;
    int32_t displacement;
    std::memcpy(&displacement, call + 1, sizeof(displacement));
    if (call + 5 + displacement != twinui.base + g_found.registerRoot.rva) return false;
    bool hooked = CreateHook(twinui, g_found.show, &ShowHook, &g_show) &&
                  CreateHook(twinui, g_found.createControl, &CreateControlHook, &g_createControl) &&
                  CreateHook(twinui, g_found.dismiss, &DismissHook, &g_dismiss) &&
                  CreateHook(twinui, g_found.display, &DisplayHook, &g_display) &&
                  CreateHook(twinui, g_found.setHostContent, &SetHostContentHook, &g_setHostContent) &&
                  CreateHook(twinui, g_found.registerRoot, &RegisterRootHook, &g_registerRoot) &&
                  CreateHook(twinui, g_found.getItemForView, &GetItemForViewHook, &g_getItemForView) &&
                  CreateHook(switcher, found.switcher.closeList, &CloseListHook, &g_closeList) &&
                  CreateHook(switcher, found.switcher.findDescendant, &FindDescendantHook, &g_findDescendant);
    if (hooked && !g_stoppedEvent) g_stoppedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return hooked && g_stoppedEvent;
}

void Enable() {
    g_activeMarker = CreateEventW(nullptr, TRUE, TRUE, protocol::ActiveMarkerName(GetCurrentProcessId()).c_str());
    g_turnedOffEvent = CreateEventW(nullptr, TRUE, FALSE, protocol::TurnedOffEventName(GetCurrentProcessId()).c_str());
    g_enabled = true;
}

bool Stop(DWORD timeoutMs) {
    AcquireSRWLockExclusive(&g_uiThreadLock);
    g_stopping = true;  // from here on no thread can become the UI thread
    HWND worker = g_worker;
    ReleaseSRWLockExclusive(&g_uiThreadLock);
    if (!worker) {
        Disable();  // no menu was ever shown, so none is kept
        return true;
    }
    g_finalStop = true;
    g_turnOffRequested = true;
    PostMessageW(worker, WM_APP, 0, 0);
    return WaitForSingleObject(g_stoppedEvent, timeoutMs) == WAIT_OBJECT_0;
}

}  // namespace persistent_menu
