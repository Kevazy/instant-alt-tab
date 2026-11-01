#include "PersistentMenu.h"

#include "CardListUpdates.h"
#include "KeptMenu.h"
#include "../shared/Log.h"
#include "../shared/Ipc.h"

#include <intrin.h>

#include <cstring>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace PersistentMenu {

ModuleImage g_twinui, g_switcher;
AltTab::TwinuiInternals g_internals;
uint32_t g_listClosed = 0;
std::optional<KeptMenu> g_kept;
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

CreateControlFn g_createControl;
ShowFn g_show;
HostFn g_dismiss, g_display;
SetHostContentFn g_setHostContent;
RegisterRootFn g_registerRoot;
CloseListFn g_closeList;
GetItemForViewFn g_getItemForView;
FindDescendantFn g_findDescendant;

void* g_dismissingHost = nullptr;
void* g_displayedHost = nullptr;  // displayed by ShowHook; another DisplayAltTab for it is a no-op
bool g_closeListSeen = false;     // Windows was seen calling closeList with a card list
std::vector<com_ptr<IUnknown>> g_listsToClose;
std::atomic<HANDLE> g_activeMarker{nullptr};  // exists while g_enabled; see Ipc::ActiveMarkerName
std::atomic<HWND> g_worker{nullptr};          // message-only window on the UI thread for deferred work
wchar_t g_workerClass[64];
std::atomic<bool> g_turnOffRequested{false}, g_finalStop{false};
HANDLE g_stoppedEvent = nullptr;
HANDLE g_turnedOffEvent = nullptr;  // see Ipc::TurnedOffEventName

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
    CardListUpdates::SetQuietVector(nullptr);
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
    void* cards = CardListUpdates::CardVectorOf(list);
    IUnknown* dataSource = ComMember(host, g_internals.hostDataSource);
    if (!cards || !dataSource) {
        Log(L"menu not kept: its card list or data source isn't the expected one");
        return;
    }
    com_ptr<IUnknown> typed;
    dataSource->QueryInterface(g_internals.switchItemDataSource, typed.put_void());
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
    CardListUpdates::SetQuietVector(cards);
}

// ---------------------------------------------------------------------------
// Hooks

HRESULT ShowHook(void* host, void* display, int flags, void* view) {
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
        SelectFirstCard(ComMember(host, g_internals.hostControl));
    }

    // Display only now, after laying out the menu, so its first frame is complete.
    if (opening.cancelled || !(opening.reused || opening.displayDeferred) || !ComMember(host, g_internals.hostWindow)) {
        return hr;
    }
    try {
        AsElement(ComMember(host, g_internals.hostControl)).UpdateLayout();
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

IUnknown* CreateControlHook(void* host) {
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
    unsigned sorts = CardListUpdates::SortCount();
    IUnknown* control = g_createControl(host);
    opening->built.copy_from(control);
    // Building the control creates its card list and sorts it once.
    if (control && CardListUpdates::SortCount() != sorts)
        Keep(control, host, CardListUpdates::LastSortedList(), opening->display);
    Log(L"menu built (%s)", blocker->c_str());
    return control;
}

void DismissHook(void* host) {
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

void SetHostContentHook(IUnknown** window, IUnknown** content) {
    HookScope scope;
    // A newly built menu must go into the host window member discovery found.
    Opening* opening = g_opening;
    if (g_enabled && opening && opening->built && *content && GetCurrentThreadId() == g_uiThread &&
        window != &MemberAt(opening->host, g_internals.hostWindow) && SameObject(*content, opening->built.get())) {
        TurnOff(L"the host doesn't hold its window where discovery found it");
    }
    if (g_enabled && g_kept && GetCurrentThreadId() == g_uiThread) {
        KeptMenu& kept = *g_kept;
        // Dismissal clears the content: keep the menu attached, hidden.
        void* host = g_dismissingHost;
        if (host && window == &MemberAt(host, g_internals.hostWindow) && !*content &&
            SameObject(ComMember(host, g_internals.hostControl), kept.control.get()) &&
            HostWindowShows(*window, kept.control.get())) {
            kept.hostWindow.copy_from(*window);
            kept.hidden = true;
            return;
        }
        // A reused opening sets the same control again: nothing to do.
        if (g_opening && g_opening->reused && window == &MemberAt(g_opening->host, g_internals.hostWindow) &&
            *content && SameObject(*content, kept.control.get()) && HostWindowShows(*window, kept.control.get())) {
            return;
        }
    }
    g_setHostContent(window, content);
}

void DisplayHook(void* host) {
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

IUnknown* RegisterRootHook(IUnknown* registrar, IUnknown* root) {
    HookScope scope;
    bool fromShow = _ReturnAddress() == g_twinui.base + g_internals.registerRootReturnInShow;
    if (!fromShow || !g_enabled || GetCurrentThreadId() != g_uiThread) return g_registerRoot(registrar, root);
    Opening* opening = g_opening;
    if (!opening || !g_kept || !SameObject(ComMember(opening->host, g_internals.hostControl), g_kept->control.get())) {
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
void* GetItemForViewHook(void* source, void* view, void* a, void* b) {
    HookScope scope;
    Opening* opening = g_opening;
    if (g_enabled && opening && GetCurrentThreadId() == g_uiThread) ConfirmBuiltControl(*opening);
    return g_getItemForView(source, view, a, b);
}

// The switcher closes a list when a card is chosen: it stops listening to the
// data source because the menu is about to be thrown away. The kept menu's list
// must keep listening, or it misses every window change from then on.
void CloseListHook(void* list) {
    HookScope scope;
    if (CardListUpdates::CardVectorOf(list)) g_closeListSeen = true;
    if (g_enabled && g_kept && g_kept->list.get() == list && GetCurrentThreadId() == g_uiThread) {
        g_kept->closeSkipped = true;
        return;
    }
    g_closeList(list);
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
        found = g_findDescendant(root);
    } catch (...) {
        --depth;
        throw;
    }
    --depth;
    return found;
}

}  // namespace

bool InstallHooks(const ModuleImage& twinui, const ModuleImage& switcher, const AltTab::Internals& found) {
    g_twinui = twinui;
    g_switcher = switcher;
    g_internals = found.twinui;
    g_listClosed = found.switcher.listClosed;
    // The one real RegisterXamlRoot call: a direct call to it right before that return address.
    const BYTE* call = twinui.base + g_internals.registerRootReturnInShow - 5;
    if (!IsCode(twinui, call) || !IsCode(twinui, call + 4) || call[0] != 0xE8) return false;
    int32_t displacement;
    std::memcpy(&displacement, call + 1, sizeof(displacement));
    if (call + 5 + displacement != twinui.base + g_internals.registerRoot.rva) return false;
    bool hooked = CreateHook(twinui, g_internals.show, &ShowHook, &g_show) &&
                  CreateHook(twinui, g_internals.createControl, &CreateControlHook, &g_createControl) &&
                  CreateHook(twinui, g_internals.dismiss, &DismissHook, &g_dismiss) &&
                  CreateHook(twinui, g_internals.display, &DisplayHook, &g_display) &&
                  CreateHook(twinui, g_internals.setHostContent, &SetHostContentHook, &g_setHostContent) &&
                  CreateHook(twinui, g_internals.registerRoot, &RegisterRootHook, &g_registerRoot) &&
                  CreateHook(twinui, g_internals.getItemForView, &GetItemForViewHook, &g_getItemForView) &&
                  CreateHook(switcher, found.switcher.closeList, &CloseListHook, &g_closeList) &&
                  CreateHook(switcher, found.switcher.findDescendant, &FindDescendantHook, &g_findDescendant);
    if (hooked && !g_stoppedEvent) g_stoppedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return hooked && g_stoppedEvent;
}

void Enable() {
    g_activeMarker = CreateEventW(nullptr, TRUE, TRUE, Ipc::ActiveMarkerName(GetCurrentProcessId()).c_str());
    g_turnedOffEvent = CreateEventW(nullptr, TRUE, FALSE, Ipc::TurnedOffEventName(GetCurrentProcessId()).c_str());
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

}  // namespace PersistentMenu
