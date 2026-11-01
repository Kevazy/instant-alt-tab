# How Instant Alt+Tab works

This is the long version of the README's "How it works": what Windows does when Alt+Tab opens, what
each hook changes, and how to support a new Windows build. All offsets mentioned here live in
[`src/hook/pins.h`](../src/hook/pins.h).

## The pieces

| File | Runs in | Job |
|---|---|---|
| `instant-alt-tab.exe` | a terminal, or double-clicked | install, uninstall, start, stop, status |
| `instant-alt-tab-agent.exe` | the background, from sign-in | loads the hooks into each shell Explorer |
| `instant-alt-tab-hook.dll` | Explorer | the hooks |

The agent finds the shell Explorer (`GetShellWindow`), waits until Explorer has loaded its Alt+Tab
modules (they load on the first Alt+Tab), loads the DLL with `CreateRemoteThread` + `LoadLibraryW`,
and calls its `Start` export. When Explorer exits it waits for the next one. `Stop` reverses
everything, and the agent unloads the DLL only once `Stop` has confirmed that no code in it can run
anymore.

## What Windows does on Alt+Tab

Two modules are involved:

- **`twinui.pcshell.dll`** hosts the view. Each opening gets a new `XamlAltTabViewHost`, whose
  `Show(display, flags, view)`:
  1. builds the menu (`CreateAltTabControl`),
  2. asks the data source for the foreground window's item (`CachedDataSource::GetItemForView`),
  3. positions the selection,
  4. puts the control into the host window and registers its XAML root,
  5. displays it (`DisplayAltTab`).

  Dismissing the menu clears the host window's content, and the control is destroyed with it.
- **`WindowsInternal.ComposableShell.Experiences.Switcher.dll`** is the menu itself: the `AltTab`
  control, a card list view model (`SwitchItemListViewModel`) that owns a vector of card view
  models, and the XAML that renders one card per vector element. The list subscribes to the shell's
  window data source and re-syncs its vector from it.

Building the control means creating every card, with its title, icon and DWM thumbnail. With ~125
windows that is ~280 ms before the first frame.

## The hooks

### Keeping the menu (`kept_menu.cpp`)

| Hooked | Why |
|---|---|
| `CreateAltTabControl` | returns the kept control instead of building one, unless something it was built for changed |
| `Show` | delays `DisplayAltTab` until the kept control has been laid out, so its first frame is complete |
| `v_DismissView` and the host-content setter | keep the control attached to the hidden host window when the menu closes |
| `DisplayAltTab` | is skipped while `Show` displays the menu itself |
| `RegisterXamlRoot` | returns the kept root's registration instead of registering it again |

The kept menu is rebuilt when the data source, the host window, the display Show targets, that
display's work area or DPI, the theme, high contrast or the window color change. On a typical
multi-monitor setup the menu always opens on the primary display, whichever monitor has focus, so
the display is read from the menu's own window, not from the focused window.

The card list is identified from the one sort the control's construction performs, and verified
at the first dismissal: the control's `ItemsSource` must be that list's vector. Until then the menu
isn't reused.

### Keeping the menu current

When a card is chosen, the switcher "closes" the list (switcher `0x99018`). That sets a flag at
`+0x12a`, which makes the list drop every data-source event, and unregisters the list's change
handler from the data source. Stock Windows throws the list away right after, so this costs nothing.
A kept list that went through it would never hear of another window change. So the hook skips it for
the kept list, which leaves the list exactly as it is after Esc. When the kept menu is dropped, the
list is closed the stock way; otherwise the data source would keep it alive.

### Quiet card edits (`card_list.cpp`)

The switcher edits the card vector one element at a time, and XAML rebuilds a card for every change
notification it gets. A re-sync after one window opens moves about 85 cards; a sort makes about
1,100 single-card replacements. The hooks on the list's re-sync and sort clear the vector's
"observed" flag (`+0x70`; the notifier raises nothing while it is clear), let the switcher edit,
then put the old contents back silently and replay only the longest-common-subsequence diff, with
notifications on. XAML sees a consistent list after every step. If the replay doesn't reproduce the
switcher's result, one `Reset` is sent instead.

### Starting each opening like a new menu

A kept control carries state from its last opening that a new control doesn't have. Each piece is
reset or made irrelevant:

- **Scroll position:** scrolled back to the top at dismissal.
- **Selection:** `Show` positions the selection in one of three ways. With the foreground window's
  item it focuses that card and moves forward, which is independent of history. For Alt+Shift+Tab
  it only moves backward, and when the foreground window has no card (the taskbar, the desktop) it
  only moves forward. Those two moves start from the control's current position, which is "none" in
  a new control and wherever the last opening ended in a kept one. The `GetItemForView` hook knows
  which branch `Show` takes, and puts the selection where a new control would end up: the last card
  for backward, the first card for the no-card case.
- **Keyboard focus:** the control's navigation methods (`NavigateToItem` and `Navigate`, which Show
  and every Tab go through) focus cards "programmatically". XAML turns that into the input mode the
  control saw last, and the menu draws its selection border only for keyboard focus. In a new
  control that's always the case. A kept control that was last dismissed with the mouse would focus
  in Pointer mode and show no border. Both methods are hooked, and after any navigation of the kept
  menu (and once more after it is displayed) the selected card is given keyboard focus.
- **The card list** is current because it stays subscribed (above).
- **Layout** matches the display because of the context check.

### A search that never finds anything (`FindDescendant`)

After every card change the switcher searches each card's subtree for an element type the Alt+Tab
menu never contains: ~470 searches and ~15 ms per opening with 125 windows. At the first dismissal
the kept menu's whole tree is checked once for that type. If it has none, searches rooted at the kept
menu's cards are answered directly.

## Safety

- **Exact build.** Both modules must match the pinned TimeDateStamp and SizeOfImage, and every
  hooked function's first 16 bytes must match. Otherwise `Start` returns "unsupported build" and
  nothing is hooked.
- **Fail closed.** Anything that contradicts what the hooks expect (`Show` failing or throwing, a
  failed layout, a card list that doesn't verify, an exception while tidying the hidden menu) drops
  the kept menu and turns retention off; Alt+Tab is stock until the hooks are started again, and
  `status` says so. A quiet edit whose replay doesn't reproduce the switcher's result isn't one of
  these: one `Reset` makes the list correct again.
- **Explorer's UI thread.** The kept menu is created, used and released only on the thread that
  shows it. Deferred work (closing a dropped list, turning off) runs from a message-only window on
  that thread, never nested inside a hooked call.
- **Unloading.** Every hook counts itself in and out (`HookScope`). `Stop` disables the hooks, waits
  for the count to reach zero, and only then frees the trampolines. If something is still running
  after the timeout, the DLL pins itself in memory, so the agent's `FreeLibrary` can't unload code
  that is in use.
- **Crash guard.** If Explorer exits with an error status twice within an hour while the hooks are
  active, the agent stops loading them until the next install. Normal restarts and sign-out don't
  count.

## Supporting another Windows build

1. Run `python tools/find_offsets.py` (needs the Debugging Tools for Windows, part of the Windows
   SDK). It finds twinui's functions by name from Microsoft's public symbols and the switcher's by
   code structure, and prints them in the layout of `pins.h`.
2. Replace the matching entries in `src/hook/pins.h`. These values aren't derived and should be
   checked by hand:
   - the host window's content getter/setter vtable slots (8 and 9);
   - the card view model's `ISwitchItem` field (`+0xd8`);
   - `kNavigateToItem` and `kNavigate`, the switcher functions at the control's vtable slots that
     the script does print (`kNavigateToItemSlot`, `kNavigateSlot`);
   - the interface IDs `kISwitchItem` and `kSearchedType`, which change only if the interfaces do.
3. Build, run `instant-alt-tab start` from the build folder, and check `hook.log`. Uninstall any
   installed copy first: the commands act on the installed copy when there is one.
4. Check, at minimum:
   - switching with Alt+Tab and Alt+Shift+Tab;
   - Esc and reopening;
   - opening, closing and closing-from-the-menu of windows, both while the menu is hidden and while
     it is shown;
   - Task View, then Alt+Tab;
   - Alt+Tab with the taskbar focused;
   - the speed, with `tools/measure_latency.cpp`.
