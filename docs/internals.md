# How Instant Alt+Tab works

This is the long version of the README's "How it works": what Windows does when Alt+Tab opens, what
each hook changes, and how the hooks find what they need in whatever Windows build they run on.
No address or offset is written down anywhere in the source;
[Finding the internals](#finding-the-internals) explains where each one comes from.

## The pieces

| File | Runs in | Job |
|---|---|---|
| `instant-alt-tab.exe` | a terminal, or double-clicked | install, uninstall, start, stop, status |
| `instant-alt-tab-agent.exe` | the background, from sign-in | loads the hooks into each shell Explorer |
| `instant-alt-tab-hook.dll` | Explorer | the hooks |

The agent finds the shell Explorer (`GetShellWindow`), waits until Explorer has loaded its Alt+Tab
modules (they load on the first Alt+Tab), works out from those DLL files what the hooks need
(`src/discovery`), loads the DLL with `CreateRemoteThread` + `LoadLibraryW`, and calls its `Start`
export with the results. When Explorer exits it waits for the next one. `Stop` reverses
everything, and the agent unloads the DLL only once `Stop` has confirmed that no code in it can run
anymore.

## What Windows does on Alt+Tab

Two modules are involved:

- **`twinui.pcshell.dll`** hosts the view. Each opening gets a new `XamlAltTabViewHost`, whose
  `Show(display, flags, view)`:
  1. builds the menu (`CreateAltTabControl`),
  2. asks the data source for the foreground window's item,
  3. positions the selection through the control's `NavigateToItem` and `Navigate`,
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

When a card is chosen, the switcher "closes" the list. That sets a flag in the list, which makes
it drop every data-source event, and unregisters the list's change handler from the data source.
Stock Windows throws the list away right after, so this costs nothing.
A kept list that went through it would never hear of another window change. So the hook skips it for
the kept list, which leaves the list exactly as it is after Esc. When the kept menu is dropped, the
list is closed the stock way; otherwise the data source would keep it alive.

### Quiet card edits (`card_list.cpp`)

The switcher edits the card vector one element at a time, and XAML rebuilds a card for every change
notification it gets. A re-sync after one window opens moves about 85 cards; a sort makes about
1,100 single-card replacements. The hooks on the list's re-sync and sort clear the vector's
"observed" flag (the notifier raises nothing while it is clear), let the switcher edit,
then put the old contents back silently and replay only the longest-common-subsequence diff, with
notifications on. XAML sees a consistent list after every step. If the replay doesn't reproduce the
switcher's result, one `Reset` is sent instead.

### Starting each opening like a new menu

A kept control carries state from its last opening that a new control doesn't have. Each piece is
reset or made irrelevant:

- **Scroll position:** scrolled back to the top at dismissal.
- **Selection:** `Show` positions the selection in one of three ways. With the foreground window's
  item it calls `NavigateToItem` and moves forward, which is independent of history. For
  Alt+Shift+Tab it only moves backward, and when the foreground window has no card (the taskbar, the
  desktop) it only moves forward. Those two moves start from the control's current position, which
  is "none" in a new control and wherever the last opening ended in a kept one. The navigation hooks
  see which call comes first and put the selection where a new control would end up: the last card
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

- **Only what was analysed.** The loaded modules must be the files discovery analysed (same
  TimeDateStamp and SizeOfImage), and every hooked function's first 16 bytes in memory must be the
  ones read from the file. Otherwise `Start` refuses and nothing is hooked.
- **Checked against Windows itself.** The first opening confirms the host's members: the control
  the factory returns must be where discovery said the host keeps it, and the window Show puts it in
  must be the host window member. A mismatch turns the hooks off.
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

## Finding the internals

The hooks need about thirty facts about two DLLs: where some functions are, the layout of a few
objects, and a few vtable slots. They change with nearly every monthly Windows update, so the agent
works them out on the user's machine, from the exact files Explorer loaded, every time it starts
the hooks (`src/discovery`). Each rule must match exactly once; if one doesn't, the hooks stay off
and the user is told once, with a message window.

**twinui.pcshell.dll** has public symbols. The first time the agent sees a build, it downloads the
PDB from Microsoft's symbol server (about 30 MB), looks up a dozen names with the system's dbghelp,
keeps just those addresses in a small file named after the PDB's signature, and deletes the PDB.
From the named functions the rest follows by reading what their code does with their parameters:

| Fact | Where it comes from |
|---|---|
| The view host's functions | their names (`XamlAltTabViewHost::Show` and so on) |
| The host's window member | the address Show passes to the window content setter |
| The host's control member | what Show calls the control's `NavigateToItem` and `Navigate` on |
| The host's data source member | the address the factory passes to `to_cx<ISwitchItemDataSource>` |
| The control's `NavigateToItem` and `Navigate` slots | the vtable slots twinui's named wrappers call |
| The window content setter slot, and the getter's | the slot the named setter calls; WinRT puts a property's getter right before its setter |
| The one real `RegisterXamlRoot` call | the only call to it in Show (the function is shared with unrelated folded code) |
| `ISwitchItem`'s interface ID | the named `guid_v<ISwitchItem>` constant |

**The switcher DLL** has no public symbols. Its functions are found by what they do, with the
instructions decoded (Zydis) and the registers followed, so recompiles that only change instruction
encodings, register use or prologues still match:

| Fact | Where it comes from |
|---|---|
| The card vector's change notifier, and its "observed" flag | a function that first tests a byte member of its parameter, and is called with `CollectionChange.ItemInserted` by one function and `ItemRemoved` by another |
| The vector's `InsertAt` and `RemoveAt` | those two callers |
| The list's re-sync, and the list's card vector member | the one function calling both on the same member of its parameter |
| The list's sort | the virtual method the re-sync calls on the list itself that reads that member |
| The vector's storage | the member `RemoveAt` reads as a begin/end pair |
| The list's close, and its closed flag | a two-instruction function that sets a byte member to 1 and jumps to a function that checks the list's subscription |
| The descendant search | the recursive function that references the searched element type's interface ID |
| The control's `NavigateToItem` and `Navigate` | read from the first built control's vtable, and hooked once Show is seen storing exactly that pointer in the host |
| Where a card keeps its `ISwitchItem` | the one card holding the item Show navigates to, checked again at every reused opening |

The only constants in the source are public contracts that don't change between Windows versions:
`CollectionChange` values from the Windows API, and one interface ID (COM never changes an
interface's ID; a changed interface gets a new one).

### Checking a Windows build

`discover.exe` (built with `cmake --build --preset release --target discover`) runs the same
discovery on any copies of the two DLLs and prints the results. Windows keeps the previous and the
next pending versions of system files under `C:\Windows\WinSxS`, so new rules can be checked against
several builds without touching Explorer:

```
discover --twinui <path>\twinui.pcshell.dll --switcher <path>\WindowsInternal.ComposableShell.Experiences.Switcher.dll
```

On a build where it works, check at minimum:

- switching with Alt+Tab and Alt+Shift+Tab;
- Esc and reopening;
- opening, closing and closing-from-the-menu of windows, both while the menu is hidden and while
  it is shown;
- Task View, then Alt+Tab;
- Alt+Tab with the taskbar focused;
- the speed, with `tools/measure_latency.cpp`.
