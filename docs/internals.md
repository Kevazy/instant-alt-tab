# How Instant Alt+Tab works

This is the long version of the README's "How it works": what Windows does when Alt+Tab opens, what
the hooks change, and how they find what they need in whatever Windows build they run on. No
address or offset is written down anywhere in the source;
[Finding the internals](#finding-the-internals) explains where they come from.

It describes ideas, not files. The code is organized by the same ideas and uses the same words
(see the [glossary](#words-used-here)), so searching the source for a term finds where it lives.

## The pieces

Three programs work together:

- **`instant-alt-tab.exe`**, the one file users download: installs, uninstalls, and reports the
  status, by double-click or from a terminal. The other two are built into it, and installing writes
  them next to it.
- **The agent** (`instant-alt-tab-agent.exe`) runs in the background from sign-in. It finds the
  shell Explorer, works out what the hooks need from the DLL files Explorer uses, and loads the hook
  DLL into it. When Explorer exits it waits for the next one.
- **The hook DLL** (`instant-alt-tab-hook.dll`) runs inside Explorer and keeps the menu.

The agent gets to work once Explorer has loaded `twinui.pcshell.dll`, which it does while
starting. Explorer would load the switcher DLL only at the first Alt+Tab, so the agent takes its
path from where Windows registered it (the `DllPath` of the WinRT classes registered to that file,
which is where Windows itself loads it from), works out what the hooks need, and loads it into
Explorer, so the hooks are usually in place before the first press. (Right after a Windows update
that waits for the symbol download; if no registration names the file, the agent waits for the
first Alt+Tab to load it.) It loads the hook DLL with `CreateRemoteThread` + `LoadLibraryW` and calls its `Start` export
with what it found. `Stop` reverses everything, and the agent unloads the DLL only once `Stop` has
confirmed that no code in it can run anymore. The switcher DLL stays loaded: it is Windows' own,
and Explorer would have loaded it at the first Alt+Tab anyway.

In the source, `src/cli`, `src/agent` and `src/hook` hold the three programs (the command-line
tool also uses some of the agent's code), `src/discovery` the agent's code that finds the internals,
`src/shared` what more than one of them uses, and `tools` developer tools. Good places to start
reading are the agent's main loop, `Discovery::Discover`, and the hook DLL's `Start`.

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
  and lets Windows build a new one, which makes that opening as slow as stock. `hook.log` records
  every rebuild with its reason (`menu built (...)`); the code that decides calls it a blocker.
- **Stock**: Windows' own Alt+Tab, with no hooks active. Instant Alt+Tab ends up stock in these ways:
  - **waiting for symbols**: right after a Windows update, until the new build's public symbols are
    downloaded. Failed downloads are retried less and less often, and the user is told once if it
    goes on. If Microsoft hasn't published them yet, the agent tells the user once and checks daily.
  - **paused**: discovery didn't recognize this Windows build, so the hooks never load; until the
    next Windows update changes the DLLs.
  - **turned off**: the hooks saw something unexpected in Explorer and stopped; until Explorer
    restarts or the hooks are started again.
  - **disabled**: Explorer crashed twice within an hour after the agent loaded anything into it;
    until the next install.
- **Discovery**: working out, from the DLL files on the user's PC, every function and offset the
  hooks need. What it finds are the **internals**. A **rule** is how one of them is found; every
  rule must match exactly once.
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

- **twinui** hosts the view. Each opening gets a new `XamlAltTabViewHost`, whose
  `Show(display, flags, view)`:
  1. builds the menu (`CreateAltTabControl`),
  2. asks the data source for the foreground window's item,
  3. positions the selection through the control's `NavigateToItem` and `Navigate`,
  4. puts the control into the host window and registers its XAML root,
  5. displays it (`DisplayAltTab`).

  Dismissing the menu clears the host window's content, and the control is destroyed with it.
- **The switcher DLL** is the menu itself: the `AltTab` control, a card list view model that owns a
  vector of card view models, and the XAML that renders one card per vector element. The list
  subscribes to the shell's window data source and re-syncs its vector from it.

Building the control means creating every card, with its title, icon and DWM thumbnail, so it takes
longer the more windows are open.

## The hooks

### Keeping the menu

The hooks on `CreateAltTabControl`, `Show`, the dismissal, the host's content setter, `DisplayAltTab`
and `RegisterXamlRoot` return the kept control instead of building one, keep it attached to the
hidden host window when the menu closes, and display it only once it has been laid out, so its
first frame is complete.

The kept menu is rebuilt when anything it was laid out for changes (the data source, the host
window, the display it opens on, that display's work area or DPI, the theme, high contrast or the
window color), and when its card list can't be trusted: not verified yet, closed or replaced, or
missing a window it should have. On a typical multi-monitor setup the menu always opens on the primary display,
whichever monitor has focus, so the display is read from the menu's own window. A monitor is
identified by its device name and bounds, not its handle: Windows hands out new handles when
monitors wake from sleep.

The card list is identified from the one sort the control's construction performs, and verified
at the first dismissal: the control's `ItemsSource` must be that list's vector. Until then the menu
isn't reused.

### Keeping the menu current

When a card is chosen, the switcher "closes" the list. That sets a flag in the list, which makes
it drop every data-source event, and unregisters the list's change handler from the data source.
Stock Windows throws the list away right after, so this costs nothing. A kept list that went
through it would never hear of another window change. So the hook skips it for the kept list,
which leaves the list exactly as it is after Esc. When the kept menu is dropped, the list is closed
the stock way; otherwise the data source would keep it alive.

### Quiet card edits

The switcher edits the card vector one element at a time, and XAML rebuilds a card for every change
notification it gets; a re-sync after one window opens can move dozens of cards, and a sort makes
hundreds of single-card replacements. The hooks on the list's re-sync and sort clear the vector's
"observed" flag (the notifier raises nothing while it is clear), let the switcher edit, then put the
old contents back silently and replay only the longest-common-subsequence diff, with notifications
on. XAML sees a consistent list after every step. If the replay doesn't reproduce the switcher's
result, one `Reset` is sent instead.

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
- **Keyboard focus:** the control's navigation methods focus cards "programmatically". XAML turns
  that into the input mode the control saw last, and the menu draws its selection border only for
  keyboard focus. A kept control that was last dismissed with the mouse would focus in Pointer mode
  and show no border, so after any navigation of the kept menu the selected card is given keyboard
  focus.
- **The card list** is current because it stays subscribed (above).
- **Layout** matches the display because of the rebuild rules (above).

### A search that never finds anything

After every card change the switcher searches each card's subtree for an element type the Alt+Tab
menu never contains, hundreds of times per opening with many windows open. At the first dismissal
the kept menu's whole tree is checked once for that type. If it has none, searches rooted at the
kept menu's cards are answered directly.

## Safety

- **Only what was analysed.** The loaded modules must be the files discovery analysed (same
  TimeDateStamp and SizeOfImage), and every function hooked from discovery's results must start
  with the bytes read from the file. Otherwise `Start` refuses and nothing is hooked. The control's
  navigation methods are taken from its live vtable instead, and hooked only once Show has been seen
  storing that control in the host.
- **Checked against Windows itself.** The first opening confirms what discovery found about the
  host: the control the factory returns must be where discovery said the host keeps it, and the
  window Show puts it in must be the host window member. A mismatch turns the hooks off.
- **Fail closed.** Anything that contradicts what the hooks expect (`Show` failing or throwing, a
  failed layout, a card list that doesn't verify, an exception while tidying the hidden menu) drops
  the kept menu and turns the hooks off; Alt+Tab is stock until they are started again, and
  `status` says so. A quiet edit whose replay doesn't reproduce the switcher's result isn't one of
  these: one `Reset` makes the list correct again.
- **Explorer's UI thread.** The kept menu is created, used and released only on the thread that
  shows it. Deferred work (closing a dropped list, turning off) runs from a message-only window on
  that thread, never nested inside a hooked call.
- **Unloading.** Every hook counts itself in and out. `Stop` disables the hooks, waits for the count
  to reach zero, and only then frees the trampolines. If something is still running after the
  timeout, the DLL pins itself in memory, so the agent's `FreeLibrary` can't unload code that is in
  use.
- **Crash guard.** If Explorer exits with an error status twice within an hour after the agent loaded
  anything into it (the switcher DLL or the hooks), the agent stops loading anything until the next
  install. Normal restarts and sign-out don't count.

## Finding the internals

The hooks need a few dozen facts about the DLLs: where some functions are, the layout of a few
objects, and a few vtable slots. They change with nearly every monthly Windows update, so the agent
works them out on the user's machine, from the exact files Explorer uses, every time it starts the
hooks. Each rule must match exactly once; if one doesn't, the hooks stay off and the user is told
once.

- **twinui** has public symbols. The first time the agent sees a build, it downloads the PDB from
  Microsoft's symbol server, looks up the names it needs with the system's dbghelp, keeps just those
  addresses in a small file named after the PDB's signature, and deletes the PDB. Member offsets and
  vtable slots follow by reading what the named functions do with their parameters.
- **The switcher DLL** has no public symbols. Its functions are found by what they do: which
  functions call which, with which constant arguments, and which members of their parameters they
  read. The instructions are decoded (Zydis) and the registers followed, so recompiles that only
  change instruction encodings, register use or prologues still match.
- **The data model DLL** gives the class of the items behind the cards, by its vtables' public
  symbol names. It is optional: it lets the hooks learn the card layout in the very first opening
  after Explorer starts, when Show selects no window. Without it they learn it one opening later.

Each rule is described next to its code. The code walker is approximate (code reached only by a
jump starts from the registers at the jumps to it), so every rule also checks that what it found is
consistent: exactly one match, members that don't overlap, the same value from every caller. What
can only be confirmed in a running Explorer is checked there, at the first opening.

Nothing that differs between Windows builds is written into the source. The few fixed values are
ones that don't: `CollectionChange` values from the Windows API, one interface ID (COM never changes
an interface's ID; a changed interface gets a new one), and the class name of the window that hosts
the menu.

### Checking a Windows build

The `discover` tool (`cmake --build --preset release --target discover`) runs the same discovery on
the DLLs and prints the results: this PC's by default, or other copies given with `--twinui`,
`--switcher` and `--datamodel`. Windows
keeps the previous and the next pending versions of system files under `C:\Windows\WinSxS`, so rules
can be checked against several builds without touching Explorer.

Other builds' files can be found without installing them:

- **twinui.pcshell.dll** and **SwitcherDataModel.dll**: [Winbindex](https://winbindex.m417z.com)
  lists every released build with its timestamp and size, and Microsoft's symbol server serves the
  file itself at
  `https://msdl.microsoft.com/download/symbols/<name>/<TIMESTAMP><SizeOfImage>/<name>`
  (timestamp as 8 hex digits, size in hex).
- **The switcher DLL**: the symbol server doesn't have it. It comes from Windows' own update
  packages: UUP dump lists every build's packages, including Insider builds, and the file can be
  extracted from them, or rebuilt from a cumulative update's deltas.
- The DLLs must be read from a path shorter than 260 characters.

On a build where it works, check at minimum:

- switching with Alt+Tab and Alt+Shift+Tab;
- Esc and reopening;
- opening, closing and closing-from-the-menu of windows, both while the menu is hidden and while
  it is shown;
- Task View, then Alt+Tab;
- Alt+Tab with the taskbar focused;
- the first Alt+Tab after Explorer restarts, and after the monitors sleep;
- the speed, with the `measure-latency` tool.
