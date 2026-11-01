// The selection each opening starts with. A kept control remembers where the
// previous opening left it, so its first move is reset to where a new menu
// would start; see NavigateToItemHook.

#include "KeptMenuState.h"

#include "QuietCardEdits.h"
#include "../shared/Log.h"

#include <algorithm>
#include <array>
#include <set>
#include <vector>

#include <winrt/Windows.UI.Xaml.Input.h>

namespace KeptMenu {
namespace {

// clang-format off
using NavigateToItemFn = HRESULT(*)(IUnknown* control, IUnknown* item);
using NavigateFn = HRESULT(*)(IUnknown* control, bool backward);
// clang-format on

NavigateToItemFn g_originalNavigateToItem;
NavigateFn g_originalNavigate;

constexpr size_t kCardScanBytes = 0x200;     // how far into a card its item is looked for
std::array<void*, 2> g_navigationTargets{};  // the control's NavigateToItem and Navigate, once hooked
thread_local bool t_selectingFirstCard = false;

// Which module an object's vtable is in, and where: for the log.
std::wstring DescribeVtable(const void* object) {
    auto vtable = static_cast<const BYTE*>(ReadPointer(object, 0));
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (!vtable ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(vtable), &module)) {
        return L"vtable outside any module";
    }
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    wchar_t text[MAX_PATH + 40];
    swprintf_s(text, L"vtable %s+0x%llx", name ? name + 1 : path,
               static_cast<unsigned long long>(vtable - reinterpret_cast<const BYTE*>(module)));
    return text;
}

com_ptr<IUnknown> SwitchItemOf(IInspectable* card) {
    com_ptr<IUnknown> item;
    auto field = g_cardItemOffset ? static_cast<IUnknown*>(ReadPointer(card, g_cardItemOffset)) : nullptr;
    if (LooksLikeComObject(field)) field->QueryInterface(g_twinuiInternals.switchItem, item.put_void());
    return item;
}

// How many cards hold `item` where cards keep their item.
size_t CardsHolding(IUnknown* item) {
    size_t holders = 0;
    for (IInspectable* card : QuietCardEdits::Cards(g_kept->cards.get())) {
        holders += ReadPointer(card, g_cardItemOffset) == item;
    }
    return holders;
}

// Learns where a card keeps its item from the item Show selects: exactly one
// card may hold that pointer, at exactly one offset.
void LearnCardItemOffset(IUnknown* item) {
    com_ptr<IUnknown> switchItem;
    item->QueryInterface(g_twinuiInternals.switchItem, switchItem.put_void());
    if (switchItem.get() != item) {
        Log(L"Show's item isn't a card's ISwitchItem; menus aren't kept");
        return;
    }
    std::set<size_t> offsets;
    size_t holders = 0;
    for (IInspectable* card : QuietCardEdits::Cards(g_kept->cards.get())) {
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
        g_cardItemOffsetConfirmed = true;
        Log(L"cards keep their item at +0x%zx (%s)", g_cardItemOffset, DescribeVtable(item).c_str());
    } else {
        Log(L"couldn't tell where cards keep their item (%zu cards hold it, at %zu offsets)", holders, offsets.size());
    }
}

// The item class's vtables in memory, if its DLL is loaded and is the file discovery analysed.
std::vector<const void*> ItemVtables() {
    std::vector<const void*> vtables;
    if (!g_dataModelInternals.itemVtables[0]) return vtables;
    auto module = FindModule(AltTab::kDataModelDll, g_dataModelInternals.module);
    if (!module) {
        Log(L"%s isn't loaded, or isn't the file discovery analysed; card layout learned later", AltTab::kDataModelDll);
        return vtables;
    }
    for (uint32_t rva : g_dataModelInternals.itemVtables) {
        if (!rva) continue;
        const BYTE* vtable = module->base + rva;
        if (!module->Contains(vtable) || IsCode(*module, vtable)) return {};
        vtables.push_back(vtable);
    }
    return vtables;
}

// Whether a value read from a card could point to an object: aligned, and in
// the user-mode address range. Anything else isn't followed.
bool PlausibleObject(const void* value) {
    static const SYSTEM_INFO info = [] {
        SYSTEM_INFO i;
        GetSystemInfo(&i);
        return i;
    }();
    if (value < info.lpMinimumApplicationAddress || value > info.lpMaximumApplicationAddress ||
        reinterpret_cast<uintptr_t>(value) % alignof(void*) != 0) {
        return false;
    }
    // Committed and readable, and not a guard page (reading one would use up a thread's stack guard).
    MEMORY_BASIC_INFORMATION memory;
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return VirtualQuery(value, &memory, sizeof(memory)) && memory.State == MEM_COMMIT && (memory.Protect & kReadable) &&
           !(memory.Protect & PAGE_GUARD);
}

bool IsKeptControl(IUnknown* control) {
    return g_enabled && GetCurrentThreadId() == g_uiThread && g_kept && g_kept->control.get() == control;
}

HRESULT NavigateToItemHook(IUnknown* control, IUnknown* item);
HRESULT NavigateHook(IUnknown* control, bool backward);

// The control's NavigateToItem and Navigate are read from its own vtable, at
// the slots twinui calls them through. That is only right for the interface
// pointer twinui calls them on, so they're hooked once the host is seen
// holding exactly the pointer the factory returned, in the first opening that
// builds a menu. Later controls must have the same ones.
bool HookNavigation(IUnknown* control) {
    std::array<void*, 2> targets{VtableSlot(control, g_twinuiInternals.navigateToItemSlot),
                                 VtableSlot(control, g_twinuiInternals.navigateSlot)};
    if (g_navigationTargets[0]) return targets == g_navigationTargets;
    if (!IsCode(g_switcherImage, targets[0]) || !IsCode(g_switcherImage, targets[1]) || targets[0] == targets[1])
        return false;
    // A Stop on another thread disables all hooks; none may be enabled after it.
    AcquireSRWLockExclusive(&g_uiThreadLock);
    bool hooked = !g_stopping && HookNow(targets[0], &NavigateToItemHook, &g_originalNavigateToItem);
    hooked = hooked && HookNow(targets[1], &NavigateHook, &g_originalNavigate);
    ReleaseSRWLockExclusive(&g_uiThreadLock);
    if (hooked) g_navigationTargets = targets;
    return hooked;
}

}  // namespace

size_t g_cardItemOffset = 0;
bool g_cardItemOffsetConfirmed = false;

// Learns where cards keep their item when Show selected no window to learn it
// from, as in the first opening after Explorer starts, before Windows' window
// list is filled. The items are objects of one class (RootSwitchItem, from
// SwitcherDataModel.dll), recognized by its vtables without calling anything:
// there must be exactly one offset at which every card holds a different
// object with the same one of those vtables, and each of those objects must
// be its own ISwitchItem interface, the pointer Show navigates to. The result
// is confirmed (or corrected) by the next item Show navigates to.
void LearnCardItemOffsetFromItemClass() {
    auto cards = QuietCardEdits::Cards(g_kept->cards.get());
    std::vector<const void*> vtables = ItemVtables();
    if (cards.size() < 2 || vtables.empty()) return;  // one card proves too little
    std::vector<size_t> offsets;
    for (size_t offset = sizeof(void*); offset < kCardScanBytes; offset += sizeof(void*)) {
        const void* vtable = nullptr;
        std::set<const void*> objects;
        bool everyCard = true;
        for (IInspectable* card : cards) {
            const void* object = ReadPointer(card, offset);
            const void* objectVtable = PlausibleObject(object) ? ReadPointer(object, 0) : nullptr;
            if (!vtable && std::find(vtables.begin(), vtables.end(), objectVtable) != vtables.end()) {
                vtable = objectVtable;
            }
            if (!vtable || objectVtable != vtable || !objects.insert(object).second) {
                everyCard = false;
                break;
            }
        }
        // Each must be its own ISwitchItem interface, the pointer Show navigates to.
        for (size_t i = 0; everyCard && i < cards.size(); ++i) {
            auto object = static_cast<IUnknown*>(ReadPointer(cards[i], offset));
            com_ptr<IUnknown> item;
            object->QueryInterface(g_twinuiInternals.switchItem, item.put_void());
            everyCard = item.get() == object;
        }
        if (everyCard) offsets.push_back(offset);
    }
    if (offsets.size() != 1) {
        Log(L"couldn't tell where cards keep their item from its class (%zu offsets fit)", offsets.size());
        return;
    }
    g_cardItemOffset = offsets.front();
    g_cardItemOffsetConfirmed = false;
    Log(L"cards keep their item at +0x%zx (from the item class, to be confirmed)", g_cardItemOffset);
}

// Moves the selection to the first card through the control's own NavigateToItem.
void SelectFirstCard(IUnknown* control) {
    auto cards = QuietCardEdits::Cards(g_kept->cards.get());
    if (cards.empty() || !control) return;
    com_ptr<IUnknown> item = SwitchItemOf(cards.front());
    if (!item) return;
    auto navigate = static_cast<NavigateToItemFn>(VtableSlot(control, g_twinuiInternals.navigateToItemSlot));
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

// Hooks the navigation once the host holds the built control itself (not
// another interface of it), or refuses the build.
void ConfirmBuiltControl(Opening& opening) {
    if (opening.confirmed || !opening.built || opening.cancelled || !g_kept || g_kept->control != opening.built) return;
    opening.confirmed = true;
    if (ReadPointer(opening.host, g_twinuiInternals.hostControl) != opening.built.get()) {
        TurnOff(L"the host doesn't hold its control where discovery found it");
    } else if (!HookNavigation(opening.built.get())) {
        TurnOff(L"the control's navigation isn't the one hooked before");
    }
}

namespace {

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
HRESULT NavigateToItemHook(IUnknown* control, IUnknown* item) {
    HookScope scope;
    Opening* opening = g_opening;
    if (opening && LooksLikeComObject(item) && !t_selectingFirstCard && IsKeptControl(control)) {
        if (!g_cardItemOffset) {
            LearnCardItemOffset(item);
        } else if (size_t holders = CardsHolding(item); holders == 1) {
            g_cardItemOffsetConfirmed = true;
        } else if (!g_cardItemOffsetConfirmed) {
            // Learned from the item class, and this item disagrees: learn it from the item instead.
            Log(L"the card layout learned from the item class doesn't fit Show's item; learning it from the item");
            g_cardItemOffset = 0;
            LearnCardItemOffset(item);
        } else if (holders > 1) {
            TurnOff(L"cards no longer hold their item where they did");
        } else {
            g_kept->missedWindow = true;  // a window too new for the kept list: rebuild next time
            Log(L"the kept card list doesn't have the foreground window yet");
        }
        if (opening->firstNavigation == FirstNavigation::None) opening->firstNavigation = FirstNavigation::ToItem;
    }
    HRESULT hr = g_originalNavigateToItem(control, item);
    if (IsKeptControl(control)) ShowSelectionAsKeyboardFocus();
    return hr;
}

HRESULT NavigateHook(IUnknown* control, bool backward) {
    HookScope scope;
    Opening* opening = g_opening;
    if (opening && IsKeptControl(control) && opening->firstNavigation == FirstNavigation::None) {
        opening->firstNavigation = backward ? FirstNavigation::Backward : FirstNavigation::Forward;
        if (backward && opening->reused) SelectFirstCard(control);
    }
    HRESULT hr = g_originalNavigate(control, backward);
    if (IsKeptControl(control)) ShowSelectionAsKeyboardFocus();
    return hr;
}

}  // namespace
}  // namespace KeptMenu
