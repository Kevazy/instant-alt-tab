#include "KeptMenu.h"

#include "QuietCardEdits.h"
#include "KeptMenuState.h"
#include "../shared/Log.h"
#include "../shared/SharedNames.h"

#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <iterator>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace KeptMenu {

ModuleImage g_twinuiImage, g_switcherImage;
AltTab::TwinuiInternals g_twinuiInternals;
uint32_t g_listClosedOffset = 0;
std::optional<MenuState> g_kept;
Opening* g_opening = nullptr;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_failed{false};
std::atomic<DWORD> g_uiThread{0};
SRWLOCK g_uiThreadLock = SRWLOCK_INIT;
bool g_stopping = false;

namespace {

// clang-format reads some of these parameter lists as multiplications.
// clang-format off
using CreateControlFn = IUnknown*(*)(void* host);
using ShowFn = HRESULT(*)(void* host, void* display, int flags, void* view);
using HostFn = void(*)(void* host);
using SetHostContentFn = void(*)(IUnknown** window, IUnknown** content);
using RegisterRootFn = IUnknown*(*)(IUnknown* registrar, IUnknown* root);
using CloseListFn = void(*)(void* list);
using GetItemForViewFn = void*(*)(void* source, void* view, void*, void*);
using FindDescendantFn = IInspectable*(*)(IInspectable* root);
// clang-format on

CreateControlFn g_originalCreateControl;
ShowFn g_originalShow;
HostFn g_originalDismiss, g_originalDisplay;
SetHostContentFn g_originalSetHostContent;
RegisterRootFn g_originalRegisterRoot;
CloseListFn g_originalCloseList;
GetItemForViewFn g_originalGetItemForView;
FindDescendantFn g_originalFindDescendant;

void* g_dismissingHost = nullptr;
void* g_displayedHost = nullptr;  // displayed by ShowHook; another DisplayAltTab for it is a no-op
bool g_closeListSeen = false;     // Windows was seen calling closeList with a card list
std::vector<com_ptr<IUnknown>> g_listsToClose;
std::atomic<HANDLE> g_activeMarker{nullptr};  // exists while g_enabled; see SharedNames::ActiveMarkerName
std::atomic<HWND> g_worker{nullptr};          // message-only window on the UI thread for deferred work
wchar_t g_workerClass[64];
std::atomic<bool> g_turnOffRequested{false}, g_finalStop{false};
HANDLE g_stoppedEvent = nullptr;
HANDLE g_turnedOffEvent = nullptr;  // see SharedNames::TurnedOffEventName

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
            g_originalCloseList(list.get());
        } catch (...) {
            Log(L"closing a dropped menu's card list failed");
        }
    }
}

void DropKeptMenu() {
    if (!g_kept) return;
    MenuState& kept = *g_kept;
    QuietCardEdits::SetQuietVector(nullptr);
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
            g_originalSetHostContent(&window, &empty);
        } catch (...) {
        }
        if (HostWindowShows(kept.hostWindow.get(), kept.control.get())) Log(L"detaching the hidden menu failed");
    }
    g_kept.reset();
}

void StopKeepingMenus() {
    g_enabled = false;
    if (HANDLE marker = g_activeMarker.exchange(nullptr)) CloseHandle(marker);
}

// Runs from the worker window, never during an opening (a drop inside Show is
// followed by the new menu taking the focus anyway).
void CompleteTurnOff() {
    StopKeepingMenus();
    if (g_failed && g_turnedOffEvent) SetEvent(g_turnedOffEvent);
    HWND focused = GetForegroundWindow();
    DropKeptMenu();
    // Detaching the hidden menu can hand the keyboard focus to its invisible window.
    HWND now = GetForegroundWindow();
    if (now != focused && focused && IsWindow(focused) && now && !IsWindowVisible(now)) SetForegroundWindow(focused);
}

}  // namespace

// Fails closed: something unexpected happened, so stop keeping menus.
void TurnOff(const wchar_t* reason) {
    Log(L"turning off: %s", reason);
    g_failed = true;
    g_turnOffRequested = true;
    if (HWND worker = g_worker) PostMessageW(worker, WM_APP, 0, 0);
}

namespace {

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
    if (g_turnOffRequested.exchange(false)) CompleteTurnOff();
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

void Keep(IUnknown* control, void* host, void* list, void* display) {
    void* cards = QuietCardEdits::CardVectorOf(list);
    IUnknown* dataSource = ComMember(host, g_twinuiInternals.hostDataSource);
    if (!cards || !dataSource) {
        Log(L"menu not kept: its card list or data source isn't the expected one");
        return;
    }
    com_ptr<IUnknown> typed;
    dataSource->QueryInterface(g_twinuiInternals.switchItemDataSource, typed.put_void());
    if (!typed) {
        TurnOff(L"the host's data source member isn't a data source");
        return;
    }
    MenuState& kept = g_kept.emplace();
    kept.display = display;
    kept.control.copy_from(control);
    kept.dataSource.copy_from(dataSource);
    kept.list.copy_from(static_cast<IUnknown*>(list));
    kept.cards.copy_from(static_cast<IUnknown*>(cards));
    QuietCardEdits::SetQuietVector(cards);
}

// ---------------------------------------------------------------------------
// Hooks

HRESULT ShowHook(void* host, void* display, int flags, void* view) {
    HookScope scope;
    if (!g_enabled || g_opening || !OnUiThread()) return g_originalShow(host, display, flags, view);

    Opening opening{host, display, flags};
    g_opening = &opening;
    g_displayedHost = nullptr;
    HRESULT hr;
    try {
        hr = g_originalShow(host, display, flags, view);
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
    bool confirmedDuringShow = opening.confirmed;
    ConfirmBuiltControl(opening);
    if (opening.built && !g_cardItemOffset && g_kept && g_kept->control == opening.built) {
        LearnCardItemOffsetFromItemClass();
    }
    if (opening.built && !g_cardItemOffset) {
        static constexpr const wchar_t* kMoves[] = {L"none", L"to the foreground window's card", L"forward",
                                                    L"backward"};
        Log(L"card layout not learned in this opening (navigation hooked %s Show; first move: %s)",
            confirmedDuringShow ? L"during" : L"after", kMoves[static_cast<int>(opening.firstNavigation)]);
    }
    // Show moved forward from the kept selection: start where a new menu starts instead.
    if (opening.reused && !opening.cancelled && opening.firstNavigation == FirstNavigation::Forward) {
        SelectFirstCard(ComMember(host, g_twinuiInternals.hostControl));
    }

    // Display only now, after laying out the menu, so its first frame is complete.
    if (opening.cancelled || !(opening.reused || opening.displayDeferred) ||
        !ComMember(host, g_twinuiInternals.hostWindow)) {
        return hr;
    }
    try {
        AsElement(ComMember(host, g_twinuiInternals.hostControl)).UpdateLayout();
    } catch (...) {
        TurnOff(L"layout failed");
    }
    g_displayedHost = host;
    g_originalDisplay(host);
    if (ShowsKeptMenu(host)) {
        OnKeptMenuDisplayed();
        ShowSelectionAsKeyboardFocus();  // displaying restores focus in the input mode last seen
    }
    return hr;
}

IUnknown* CreateControlHook(void* host) {
    HookScope scope;
    if (!g_enabled || GetCurrentThreadId() != g_uiThread || !g_opening || g_opening->host != host) {
        return g_originalCreateControl(host);
    }
    Opening* opening = g_opening;
    auto blocker = ReuseBlocker(host, opening->display);
    if (!blocker) {
        opening->reused = true;
        g_kept->control->AddRef();
        return g_kept->control.get();
    }
    DropKeptMenu();
    unsigned sorts = QuietCardEdits::SortCount();
    IUnknown* control = g_originalCreateControl(host);
    opening->built.copy_from(control);
    // Building the control creates its card list and sorts it once.
    if (control && QuietCardEdits::SortCount() != sorts)
        Keep(control, host, QuietCardEdits::LastSortedList(), opening->display);
    Log(L"menu built (%s)", blocker->c_str());
    return control;
}

void DismissHook(void* host) {
    HookScope scope;
    if (!g_enabled || GetCurrentThreadId() != g_uiThread) return g_originalDismiss(host);
    if (g_opening && g_opening->host == host) g_opening->cancelled = true;
    void* previous = g_dismissingHost;
    g_dismissingHost = host;
    try {
        g_originalDismiss(host);
    } catch (...) {
        g_dismissingHost = previous;
        TurnOff(L"dismiss threw an exception");
        throw;
    }
    g_dismissingHost = previous;
    if (g_displayedHost == host) g_displayedHost = nullptr;
    if (g_kept && g_kept->hidden) TidyHiddenMenu();
}

void SetHostContentHook(IUnknown** window, IUnknown** content) {
    HookScope scope;
    // A newly built menu must go into the host window member discovery found.
    Opening* opening = g_opening;
    if (g_enabled && opening && opening->built && *content && GetCurrentThreadId() == g_uiThread &&
        window != &MemberAt(opening->host, g_twinuiInternals.hostWindow) &&
        SameObject(*content, opening->built.get())) {
        TurnOff(L"the host doesn't hold its window where discovery found it");
    }
    if (g_enabled && g_kept && GetCurrentThreadId() == g_uiThread) {
        MenuState& kept = *g_kept;
        // Dismissal clears the content: keep the menu attached, hidden.
        void* host = g_dismissingHost;
        if (host && window == &MemberAt(host, g_twinuiInternals.hostWindow) && !*content &&
            SameObject(ComMember(host, g_twinuiInternals.hostControl), kept.control.get()) &&
            HostWindowShows(*window, kept.control.get())) {
            kept.hostWindow.copy_from(*window);
            kept.hidden = true;
            return;
        }
        // A reused opening sets the same control again: nothing to do.
        if (g_opening && g_opening->reused && window == &MemberAt(g_opening->host, g_twinuiInternals.hostWindow) &&
            *content && SameObject(*content, kept.control.get()) && HostWindowShows(*window, kept.control.get())) {
            return;
        }
    }
    g_originalSetHostContent(window, content);
}

void DisplayHook(void* host) {
    HookScope scope;
    if (!g_enabled || GetCurrentThreadId() != g_uiThread) return g_originalDisplay(host);
    if (g_opening && g_opening->host == host) {
        g_opening->displayDeferred = true;  // displayed after layout, at the end of ShowHook
        return;
    }
    if (g_displayedHost == host) return;
    g_originalDisplay(host);
    if (ShowsKeptMenu(host)) OnKeptMenuDisplayed();
}

IUnknown* RegisterRootHook(IUnknown* registrar, IUnknown* root) {
    HookScope scope;
    bool fromShow = _ReturnAddress() == g_twinuiImage.base + g_twinuiInternals.registerRootReturnInShow;
    if (!fromShow || !g_enabled || GetCurrentThreadId() != g_uiThread) return g_originalRegisterRoot(registrar, root);
    Opening* opening = g_opening;
    if (!opening || !g_kept ||
        !SameObject(ComMember(opening->host, g_twinuiInternals.hostControl), g_kept->control.get())) {
        return g_originalRegisterRoot(registrar, root);
    }
    MenuState& kept = *g_kept;
    if (kept.registration && SameObject(root, kept.root.get()) && SameObject(registrar, kept.registrar.get())) {
        kept.registration->AddRef();
        return kept.registration.get();
    }
    IUnknown* registration = g_originalRegisterRoot(registrar, root);
    if (registration) {
        kept.registration.copy_from(registration);
        kept.root.copy_from(root);
        kept.registrar.copy_from(registrar);
    }
    return registration;
}

// Show calls this after storing the control it built and before positioning
// the selection: the moment to confirm the control and hook its navigation.
void* GetItemForViewHook(void* source, void* view, void* a, void* b) {
    HookScope scope;
    Opening* opening = g_opening;
    if (g_enabled && opening && GetCurrentThreadId() == g_uiThread) ConfirmBuiltControl(*opening);
    return g_originalGetItemForView(source, view, a, b);
}

// The switcher closes a list when a card is chosen: it stops listening to the
// data source because the menu is about to be thrown away. The kept menu's list
// must keep listening, or it misses every window change from then on.
void CloseListHook(void* list) {
    HookScope scope;
    if (QuietCardEdits::CardVectorOf(list)) g_closeListSeen = true;
    if (g_enabled && g_kept && g_kept->list.get() == list && GetCurrentThreadId() == g_uiThread) {
        g_kept->closeSkipped = true;
        return;
    }
    g_originalCloseList(list);
}

// After each card change the switcher searches every card's subtree for an
// element type the Alt+Tab menu never contains (~470 searches, ~15 ms per
// opening with 125 cards). For the kept menu's cards, checked once to contain
// no such element, the answer is known.
IInspectable* FindDescendantHook(IInspectable* root) {
    HookScope scope;
    static thread_local int depth = 0;  // the search is recursive; only the outermost call is answered
    if (depth == 0 && g_enabled && IsKeptCardContainer(root)) return nullptr;
    ++depth;
    IInspectable* found;
    try {
        found = g_originalFindDescendant(root);
    } catch (...) {
        --depth;
        throw;
    }
    --depth;
    return found;
}

}  // namespace

bool InstallHooks(const ModuleImage& twinui, const ModuleImage& switcher, const AltTab::Internals& found) {
    g_twinuiImage = twinui;
    g_switcherImage = switcher;
    g_twinuiInternals = found.twinui;
    g_listClosedOffset = found.switcher.listClosed;
    // The one real RegisterXamlRoot call: a direct call to it right before that return address.
    const BYTE* call = twinui.base + g_twinuiInternals.registerRootReturnInShow - 5;
    if (!IsCode(twinui, call) || !IsCode(twinui, call + 4) || call[0] != 0xE8) return false;
    int32_t displacement;
    std::memcpy(&displacement, call + 1, sizeof(displacement));
    if (call + 5 + displacement != twinui.base + g_twinuiInternals.registerRoot.rva) return false;
    // The item class's vtables are read-only data in twinui, or they aren't used.
    for (uint32_t rva : g_twinuiInternals.switchItemVtables) {
        if (rva && (!twinui.Contains(twinui.base + rva) || IsCode(twinui, twinui.base + rva))) {
            std::fill(std::begin(g_twinuiInternals.switchItemVtables), std::end(g_twinuiInternals.switchItemVtables),
                      0);
            Log(L"the item class's vtables aren't where discovery found them; not used");
            break;
        }
    }
    bool hooked =
        CreateHook(twinui, g_twinuiInternals.show, &ShowHook, &g_originalShow) &&
        CreateHook(twinui, g_twinuiInternals.createControl, &CreateControlHook, &g_originalCreateControl) &&
        CreateHook(twinui, g_twinuiInternals.dismiss, &DismissHook, &g_originalDismiss) &&
        CreateHook(twinui, g_twinuiInternals.display, &DisplayHook, &g_originalDisplay) &&
        CreateHook(twinui, g_twinuiInternals.setHostContent, &SetHostContentHook, &g_originalSetHostContent) &&
        CreateHook(twinui, g_twinuiInternals.registerRoot, &RegisterRootHook, &g_originalRegisterRoot) &&
        CreateHook(twinui, g_twinuiInternals.getItemForView, &GetItemForViewHook, &g_originalGetItemForView) &&
        CreateHook(switcher, found.switcher.closeList, &CloseListHook, &g_originalCloseList) &&
        CreateHook(switcher, found.switcher.findDescendant, &FindDescendantHook, &g_originalFindDescendant);
    if (hooked && !g_stoppedEvent) g_stoppedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return hooked && g_stoppedEvent;
}

void Enable() {
    g_activeMarker = CreateEventW(nullptr, TRUE, TRUE, SharedNames::ActiveMarkerName(GetCurrentProcessId()).c_str());
    g_turnedOffEvent =
        CreateEventW(nullptr, TRUE, FALSE, SharedNames::TurnedOffEventName(GetCurrentProcessId()).c_str());
    g_enabled = true;
}

bool Stop(DWORD timeoutMs) {
    AcquireSRWLockExclusive(&g_uiThreadLock);
    g_stopping = true;  // from here on no thread can become the UI thread
    HWND worker = g_worker;
    ReleaseSRWLockExclusive(&g_uiThreadLock);
    if (!worker) {
        StopKeepingMenus();  // no menu was ever shown, so none is kept
        return true;
    }
    g_finalStop = true;
    g_turnOffRequested = true;
    PostMessageW(worker, WM_APP, 0, 0);
    return WaitForSingleObject(g_stoppedEvent, timeoutMs) == WAIT_OBJECT_0;
}

}  // namespace KeptMenu
