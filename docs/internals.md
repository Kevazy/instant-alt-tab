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

The agent finds the shell Explorer (`GetShellWindow`) and waits until it has loaded
`twinui.pcshell.dll`, which it does while starting. Explorer loads the switcher DLL only on the
first Alt+Tab, so the agent takes its path from where Windows registered it (the `DllPath` of the
WinRT classes registered to that file, which is where Windows itself loads it from). It works out
from the two DLL files what the hooks need (`src/discovery`), loads the switcher DLL into Explorer
if it isn't yet, so the hooks are in place for the first Alt+Tab, then loads the hook DLL with
`CreateRemoteThread` + `LoadLibraryW` and calls its `Start` export with the results. When Explorer
exits it waits for the next one. `Stop` reverses everything, and the agent unloads the DLL only
once `Stop` has confirmed that no code in it can run anymore. The switcher DLL stays loaded: it is
Windows' own, and Explorer would have loaded it at the first Alt+Tab anyway.

## The source

| Folder | Becomes | What's in it |
|---|---|---|
| `src/cli` | `instant-alt-tab.exe` | the commands (`main.cpp`), installing and uninstalling (`Installer`), starting and stopping the agent (`AgentClient`), console output (`Console`) |
| `src/agent` | `instant-alt-tab-agent.exe` | the loop that serves each Explorer (`main.cpp`); finding Explorer and loading, starting and stopping the hook DLL in it (`ExplorerProcess`); finding the switcher DLL where Windows registered it (`RegisteredDllPath`); turning everything off after repeated Explorer crashes (`CrashGuard`); the message window, shown once per message and Windows build (`Alert`) |
| `src/discovery` | part of the agent | finding what the hooks need: the entry point (`Discovery`), the rules for each DLL (`FindTwinuiInternals`, `FindSwitcherInternals`, `FindInternals.h`), downloading and reading public symbols (`Symbols`), reading DLL files (`PeFile`), following what machine code does (`CodeAnalysis`) |
| `src/hook` | `instant-alt-tab-hook.dll` | the `Start` and `Stop` exports (`DllMain.cpp`), keeping the menu: its hooks and life (`KeptMenu`, with the state its files share in `KeptMenuState.h`), whether it still fits an opening (`KeptMenuChecks`), the selection each opening starts with (`MenuSelection`); quiet card edits (`QuietCardEdits`); reading COM objects at discovered offsets (`ComObjects`); the layer over MinHook (`Hooking`) |
| `src/shared` | all three | what discovery hands the hooks (`AltTabInternals.h`), the names and codes the three programs agree on (`SharedNames.h`, `HookExports.h`), the log (`Log`), small text files (`TextFile`) |
| `tools` | built on request | `Discover` runs discovery on any copies of the two DLLs; `MeasureLatency` measures how long the menu takes to appear |

[CONTRIBUTING.md](../CONTRIBUTING.md) has the code style and a checklist for adding a discovered value.

### Where things are decided

| Question | Where |
|---|---|
| Can the kept menu be shown for this opening, or must it be rebuilt? | `ReuseBlocker` in `src/hook/KeptMenuChecks.cpp` |
| What a kept menu was laid out for (monitor, DPI, theme...) | `DisplayContext` in `KeptMenuState.h`, read by `ReadContext` |
| Which card an opening starts on | `NavigateToItemHook` and `NavigateHook` in `src/hook/MenuSelection.cpp` |
| When the hooks turn themselves off | every `TurnOff(reason)` call; the reason goes to `hook.log` |
| Where the switcher DLL's file is, before Explorer loads it | `RegisteredDllPath` in `src/agent/RegisteredDllPath.cpp` |
| When the hooks start in a new Explorer | `ServeExplorer` and `StartHooks` in `src/agent/main.cpp` |
| Whether an Explorer exit counts as a crash | `CrashGuard::IsCrash` in `src/agent/CrashGuard.h` |
| Which messages the user has seen | `Alert::ShowOnce` in `src/agent/Alert.cpp` |
| How often a failed symbol download is retried, and when the user is told | `kDownloadRetryFirstMs`, `kDownloadRetryMaxMs` and `kUnpublishedRetryMs` in `src/agent/main.cpp` |
| Keyboard focus and the selection border on a reused menu | `ShowSelectionAsKeyboardFocus` in `src/hook/MenuSelection.cpp` |

### Files next to the agent

The agent and the hook DLL keep a few small files in the folder they run from (the install folder,
`%LOCALAPPDATA%\Programs\InstantAltTab`); `SharedNames.h` names them all.

| File | Written by | Holds |
|---|---|---|
| `agent.log`, `hook.log` | the agent, the hook DLL | events worth seeing later: starts, stops, rebuilds and why, failures |
| `problem.txt` | the agent | why Alt+Tab is stock right now, shown by `status` |
| `notified.txt` | the agent | which messages were shown, so each appears once per Windows build |
| `crash-guard.txt` | the agent | recent Explorer crashes, and whether the crash guard disabled it |
| `symbols\` | discovery | the function names looked up in Windows' public symbols, per twinui build |

## Words used here

- **Explorer**: `explorer.exe`, the process that draws the taskbar, Start and Alt+Tab (the shell).
  File Explorer windows can live in it too, but here it always means the shell's process.
- **twinui**: `twinui.pcshell.dll`, the Windows DLL that hosts the Alt+Tab view. Microsoft publishes
  its symbols.
- **The switcher DLL**: `WindowsInternal.ComposableShell.Experiences.Switcher.dll`, the menu itself:
  the `AltTab` control and its card list. It has no public symbols.
- **The data model DLL**: `SwitcherDataModel.dll`, which implements the items behind the cards.
- **Card**: one window's tile in the menu. The shell's object behind it is an *item* (`ISwitchItem`).
- **Card list**: the switcher's list of cards, which XAML displays and the shell keeps up to date.
- **Opening**: one appearance of the menu, from Alt+Tab until it closes.
- **Kept menu**: the control Instant Alt+Tab keeps alive between openings. A **rebuild** drops it
  and lets Windows build a new one, which makes that opening as slow as stock.
- **Stock**: Windows' own Alt+Tab, with no hooks active. Instant Alt+Tab ends up stock in these ways:
  - **waiting for symbols**: right after a Windows update, until the new build's public symbols are
    downloaded. A failed download is retried after a minute, then less often up to hourly; after
    about 15 minutes the user is told once. If Microsoft hasn't published them yet, the agent tells
    the user once and checks daily.
  - **paused**: discovery didn't recognize this Windows build, so the hooks never load; until the
    next Windows update changes the DLLs.
  - **turned off**: the hooks saw something unexpected in Explorer and stopped; until Explorer
    restarts.
  - **disabled**: Explorer crashed twice within an hour after the agent loaded anything into it;
    until the next install.
- **Discovery**: working out, from the DLL files on the user's PC, every function and offset the
  hooks need. What it finds are the **internals** (`AltTabInternals.h`). A **rule** is how one of
  them is found; every rule must match exactly once.
- **PE file**: the file format of Windows executables and DLLs. An **RVA** (relative virtual
  address) is an offset from where the DLL is loaded in memory.
- **Public symbols**: a PDB file Microsoft publishes for most Windows DLLs, which names their
  functions. Downloaded once per Windows update from Microsoft's symbol server.
- **Vtable** and **slot**: a COM object's table of method pointers, and a method's index in it.
  An **interface ID** names a COM interface and never changes.
- **Hook**, **detour**, **trampoline**: a hook redirects a function to a replacement (the detour);
  the trampoline is how the detour still calls the original.

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

### Keeping the menu (`KeptMenu.cpp`, `KeptMenuChecks.cpp`)

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
the display is read from the menu's own window, not from the focused window. A monitor is
identified by its device name and bounds, not its handle: Windows hands out new handles when
monitors wake from sleep, and comparing handles rebuilt the menu after every display sleep.

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

### Quiet card edits (`QuietCardEdits.cpp`)

The switcher edits the card vector one element at a time, and XAML rebuilds a card for every change
notification it gets. A re-sync after one window opens moves about 85 cards; a sort makes about
1,100 single-card replacements. The hooks on the list's re-sync and sort clear the vector's
"observed" flag (the notifier raises nothing while it is clear), let the switcher edit,
then put the old contents back silently and replay only the longest-common-subsequence diff, with
notifications on. XAML sees a consistent list after every step. If the replay doesn't reproduce the
switcher's result, one `Reset` is sent instead.

### Starting each opening like a new menu (`MenuSelection.cpp`)

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
- **Crash guard.** If Explorer exits with an error status twice within an hour after the agent loaded
  anything into it (the switcher DLL or the hooks), the agent stops loading anything until the next
  install. Normal restarts and sign-out don't
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
| Where a card keeps its `ISwitchItem` | the one card holding the item Show navigates to; or, when Show selects no window (the first opening after Explorer starts, before Windows' window list is filled), the one offset at which every card holds a different item object, recognized by its class's vtable (below). Checked again at every opening that navigates to an item |

**SwitcherDataModel.dll** implements the items behind the cards (the class `RootSwitchItem`). Its
small PDB (under 1 MB) gives the vtables of that class, by the name `RootSwitchItem::vftable`. They
are optional: without them the card layout is learned one opening later, from an item Show
navigates to, so the second Alt+Tab after Explorer starts rebuilds the menu once (agent.log says
so).

The code walker follows a function from its start, and code reached only by a jump starts from
the registers at the jumps to it. That is approximate, so every rule also checks that what it found
is consistent (exactly one match, members that don't overlap, the same value from every caller).

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

Other builds' files can be found without installing them:

- **twinui.pcshell.dll**: [Winbindex](https://winbindex.m417z.com) lists every released build with
  its timestamp and size, and Microsoft's symbol server serves the file itself at
  `https://msdl.microsoft.com/download/symbols/twinui.pcshell.dll/<TIMESTAMP><SizeOfImage>/twinui.pcshell.dll`
  (timestamp as 8 hex digits, size in hex).
- **The switcher DLL**: the symbol server doesn't have it. It comes from Windows' own update
  packages: UUP dump lists every build's packages, including Insider builds, and the file can be
  extracted from them, or rebuilt from a cumulative update's deltas.
- Both DLLs must be read from a path shorter than 260 characters.

On a build where it works, check at minimum:

- switching with Alt+Tab and Alt+Shift+Tab;
- Esc and reopening;
- opening, closing and closing-from-the-menu of windows, both while the menu is hidden and while
  it is shown;
- Task View, then Alt+Tab;
- Alt+Tab with the taskbar focused;
- the speed, with `tools/MeasureLatency.cpp`.
