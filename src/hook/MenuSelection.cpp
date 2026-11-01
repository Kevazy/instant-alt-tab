// The selection each opening starts with. A kept control remembers where the
// previous opening left it, so its first move is reset to where a new menu
// would start; see NavigateToItemHook.

#include "KeptMenuState.h"

#include "QuietCardEdits.h"
#include "../shared/Log.h"

#include <array>
#include <set>

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
        Log(L"cards keep their item at +0x%zx", g_cardItemOffset);
    } else {
        Log(L"couldn't tell where cards keep their item (%zu cards hold it, at %zu offsets)", holders, offsets.size());
    }
}

// Whether `object` is a SwitchItem, by its vtable: nothing is called on it.
bool IsSwitchItem(const void* object) {
    void* vtable = object ? ReadPointer(object, 0) : nullptr;
    if (!vtable) return false;
    for (uint32_t rva : g_twinuiInternals.switchItemVtables) {
        if (rva && vtable == g_twinuiImage.base + rva) return true;
    }
    return false;
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

// Learns where cards keep their item when Show selected no window to learn it
// from, as in the first opening after Explorer starts, before Windows' window
// list is filled: every card must hold a SwitchItem (recognized by its vtable)
// at one and the same offset, and it must be the item's ISwitchItem interface.
void LearnCardItemOffsetFromItemClass() {
    auto cards = QuietCardEdits::Cards(g_kept->cards.get());
    if (cards.empty() || !g_twinuiInternals.switchItemVtables[0]) return;
    std::set<size_t> offsets;
    size_t holders = 0;
    for (IInspectable* card : cards) {
        bool holds = false;
        for (size_t offset = sizeof(void*); offset < kCardScanBytes; offset += sizeof(void*)) {
            if (IsSwitchItem(ReadPointer(card, offset))) {
                offsets.insert(offset);
                holds = true;
            }
        }
        holders += holds;
    }
    if (holders != cards.size() || offsets.size() != 1) {
        Log(L"couldn't tell where cards keep their item from its class (%zu of %zu cards hold one, at %zu offsets)",
            holders, cards.size(), offsets.size());
        return;
    }
    auto object = static_cast<IUnknown*>(ReadPointer(cards.front(), *offsets.begin()));
    com_ptr<IUnknown> item;
    object->QueryInterface(g_twinuiInternals.switchItem, item.put_void());
    if (item.get() != object) {
        Log(L"the SwitchItem cards hold isn't their ISwitchItem; card layout not learned");
        return;
    }
    g_cardItemOffset = *offsets.begin();
    Log(L"cards keep their item at +0x%zx (found from the item class)", g_cardItemOffset);
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
        } else if (size_t holders = CardsHolding(item); holders > 1) {
            TurnOff(L"cards no longer hold their item where they did");
        } else if (holders == 0) {
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
