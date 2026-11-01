# Instant Alt+Tab

Windows 11 rebuilds the whole Alt+Tab menu every time you press Alt+Tab. With a lot of windows open,
that takes long enough to feel: the menu pops in late, and fast switching stutters. Instant Alt+Tab
keeps the menu alive between presses, so it opens in about 45 ms instead of about 280 ms. The menu
itself is unchanged: same look, same order, same keys.

| Held Alt+Tab, 125 windows | Menu visible | Fully drawn |
|---|---|---|
| Windows 11 | 279 ms | 334 ms |
| Instant Alt+Tab | 44 ms | 58 ms |

*Median of 20 openings, 2560×1440, measured with [`tools/MeasureLatency.cpp`](tools/MeasureLatency.cpp).*

## Why Alt+Tab is slow

The Alt+Tab menu is a XAML control with one card per window: title, icon and live thumbnail. Windows
creates that control when you press Alt+Tab and destroys it when the menu closes. Building it costs a
few milliseconds per card, so the delay grows with every window you keep open. At around a hundred
windows (browser windows add up quickly), the delay is easy to notice.

## How it works

A small agent loads a DLL into Explorer, which owns Alt+Tab. The DLL hooks a handful of internal
functions:

- **The menu is kept, not rebuilt.** When Alt+Tab closes, the control stays attached to its hidden
  window, and the next press shows it again.
- **It stays current while hidden.** Windows normally disconnects the menu's card list from the
  window list once you pick a window, since it's about to throw the menu away. The kept list stays
  connected, so windows opening, closing and changing order show up as they do in a new menu.
- **Updates touch only what changed.** Windows edits the card list one card at a time, and XAML
  redraws a card for every edit. Those edits run silently and are replayed as the smallest set of
  insertions and removals.
- **Each opening starts like a new menu.** The selection, scroll position and focus a kept menu
  carries over from last time are reset, so it behaves exactly like a freshly built one.
- **It rebuilds when it has to:** when the monitor it opens on, its resolution, work area or DPI,
  the theme, high contrast or the window colors change. Monitors going to sleep and waking up don't
  count.

The menu that is kept has to be built once, so the first Alt+Tab after installing, and the first
after Explorer starts (at sign-in, or after a restart), open at the usual speed. Windows loads part
of its Alt+Tab code only on the first press; the agent loads it as soon as Explorer starts, so the
hooks are ready for that first press. (Right after a Windows update the agent first downloads the
new build's symbols, so that can take a little longer.)

[docs/internals.md](docs/internals.md) explains each hook and how the internals are found. If you
want to work on the code, start with [CONTRIBUTING.md](CONTRIBUTING.md).

## Install

Requires Windows 11 (see [Compatibility](#compatibility)). No administrator rights.

Download `instant-alt-tab.exe` from the [latest release](../../releases/latest), double-click it and
confirm. It isn't signed, so SmartScreen may warn the first time; choose "More info", then "Run
anyway".

It installs to `%LOCALAPPDATA%\Programs\InstantAltTab`, starts right away, and starts again at every
sign-in. If Explorer restarts or crashes, it hooks the new Explorer automatically. To update, do the
same with the new release; it replaces the installed copy.

## Uninstall

Uninstall it from **Settings > Apps > Installed apps** like any other app. This removes the hooks
from the running Explorer, the sign-in entry, and the installed files.

## Compatibility

Explorer's internals change with nearly every Windows update, so nothing about them is written into
Instant Alt+Tab. Each time it starts, it works out what it needs from the exact Explorer files on
your machine: one of the two DLLs involved by Microsoft's public symbol names, the other by what its
code does ([details](docs/internals.md#finding-the-internals)). Updates that only move code around
are handled without a new release.

If an update changes Alt+Tab in a way it doesn't recognize, it leaves Alt+Tab alone and shows a
message once. The same happens, with a daily check, in the rare case Microsoft
hasn't published a build's symbols yet.

What had been checked as of November 2026:

- **Windows 11 24H2 and 25H2** (builds 26100 and 26200): discovery finds everything in every
  monthly update from November 2024 to October 2026, and the hooks were tested live on 25H2. Builds from before November 2024 have a differently built Alt+Tab, and stay unchanged.
- **Windows 11 26H1** (28000) and **Insider builds** up to 29698: discovery finds everything,
  wherever Microsoft has published the symbols. Not tested live.
- **Windows 11 22H2/23H2**: Alt+Tab is built differently there, and stays unchanged. Windows 10
  wasn't tested.

## Safety

- **Nothing is hooked unless everything is found.** Every value must be found exactly once, the
  code in memory must be the code that was analysed, and the hooks that depend on the
  layout of Explorer's objects go in only after the first Alt+Tab has confirmed that layout against
  what Windows actually passes around.
- **It turns itself off when in doubt.** If Windows does anything unexpected (a failed call, a card
  list that isn't the one the menu shows, an exception), it drops the kept menu, Alt+Tab stays
  stock until Explorer restarts, and a message tells you.
- **It stops for good after repeated crashes.** If Explorer crashes twice within an hour after the
  agent loaded anything into it, the agent stops loading anything until you install again.
- **Stopping and uninstalling remove every hook from the running Explorer.** If a hooked call is
  still running, the DLL stays loaded but inactive until Explorer restarts.

Some antivirus software flags programs that load code into other processes. The full source is here
if you want to check what it does or build it yourself.

## Footprint

- **Disk:** about 3 MB installed, plus a few KB of looked-up symbol names per Windows build.
- **Network:** once per Windows update, the agent downloads the public symbols of twinui.pcshell.dll
  and SwitcherDataModel.dll (together about 30 MB) from Microsoft's symbol server, keeps the few
  names it needs and deletes them. Nothing else is downloaded or sent.
- **Explorer:** keeps one Alt+Tab menu alive instead of building one per press; it is the same menu
  Windows builds every time you press Alt+Tab. Over a 30-opening test with windows opening and
  closing, Explorer's memory and handle count stayed flat. Windows' Alt+Tab menu DLL is loaded soon after
  Explorer starts rather than at the first Alt+Tab.
- **Background:** the agent waits for Explorer to exit, and checks every few seconds for a new one
  while there is none. Working out the internals takes a fraction of a second per Explorer start
  once the symbols are known.

## Building

Requires Visual Studio 2022 17.5 or later with the "Desktop development with C++" workload, which
includes CMake and the Windows SDK.

```
cmake --preset default
cmake --build --preset release
```

`instant-alt-tab.exe` ends up in `build\bin\Release`, next to the agent and the hook DLL it
carries. To try it without installing, run `instant-alt-tab start` there (with no installed copy,
since commands prefer that one). MinHook and Zydis are downloaded during configuration. `--target
discover` builds a tool that runs the discovery on any copies of the DLLs
([docs](docs/internals.md#checking-a-windows-build)).

## Troubleshooting

Double-clicking `instant-alt-tab.exe` again shows the current state, including why Alt+Tab is
stock if it is. From a terminal, `instant-alt-tab status` shows the same; run it without a command
for the others (`install`, `uninstall`, `start`, and `stop`, which turns it off until the next
sign-in). The agent writes `agent.log` and the hooks write `hook.log` to the install folder; both
record starts, stops, menu rebuilds and anything unexpected.

## License

[MIT](LICENSE). It includes MinHook and Zydis, under their own licenses
([THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt)).
