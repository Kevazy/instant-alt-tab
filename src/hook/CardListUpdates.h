#pragma once

// The Alt+Tab card list: a list view model owns a vector of card view models
// that XAML observes. The switcher edits that vector one element at a time
// (a re-sync after one window opens moves ~85 cards, a sort makes ~1,100
// single-card replacements) and XAML rebuilds a card for every notification.
// The hooks here let those edits run with notifications off and then replay
// only the minimal removals and insertions, for the kept menu's list.

#include "Hooking.h"

#include <inspectable.h>

#include <span>

namespace CardListUpdates {

bool InstallHooks(const ModuleImage& switcher, const AltTab::SwitcherInternals& found);

// The card vector of a list view model, if it looks valid.
void* CardVectorOf(void* list);
std::span<IInspectable* const> Cards(void* cardVector);

// The list view model this thread sorted most recently, and how many sorts
// this thread has started: a factory call that sorted built a new list, and
// this is how it's found.
void* LastSortedList();
unsigned SortCount();

// The one card vector whose edits are replayed quietly (the kept menu's), or
// null for none. Other lists (fresh menus, Task View) are left to the switcher.
void SetQuietVector(void* cardVector);

}  // namespace CardListUpdates
