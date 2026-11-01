# Instant Alt+Tab

Windows 11 rebuilds the whole Alt+Tab menu every time you press Alt+Tab. With a lot of windows open,
that takes long enough to feel: the menu pops in late, and fast switching stutters. Instant Alt+Tab
keeps the menu alive between presses, so it opens in about 45 ms instead of about 280 ms. The menu
itself is unchanged: same look, same order, same keys.

| Held Alt+Tab, 125 windows | Menu visible | Fully drawn |
|---|---|---|
| Windows 11 | 279 ms | 334 ms |
| Instant Alt+Tab | 44 ms | 58 ms |

*Median of 20 openings, 2560×1440, measured with [`tools/measure_latency.cpp`](tools/measure_latency.cpp).*

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
- **Updates touch only what changed.** Windows edits the card list one card at a time (one new
  window moves about 85 cards, a sort makes about 1,100 replacements), and XAML redraws a card for
  every edit. Those edits run silently and are replayed as the smallest set of insertions and
  removals.
- **Each opening starts like a new menu.** The selection, scroll position and focus a kept menu
  carries over from last time are reset, so it behaves exactly like a freshly built one.
- **It rebuilds when it has to:** when the display, resolution, DPI, theme or contrast settings
  change.

The menu that is kept has to be built once, so the first Alt+Tab after installing opens at the usual
speed. After Explorer starts (at sign-in, or after a restart), the first two do: Windows loads its
Alt+Tab code only on the first press, and the hooks can attach only after that.
[docs/internals.md](docs/internals.md) explains each hook and how the internals are found.

## Install

Requires Windows 11 (see [Compatibility](#compatibility)). No administrator rights.

1. Download the zip from the [latest release](../../releases/latest) and unzip it.
2. Double-click `instant-alt-tab.exe` and confirm. Or, from a terminal in that folder:

   ```
   .\instant-alt-tab install
   ```

The executables aren't signed, so SmartScreen may warn the first time; choose "More info", then
"Run anyway".

It installs to `%LOCALAPPDATA%\Programs\InstantAltTab`, starts right away, and starts again at every
sign-in. If Explorer restarts or crashes, the agent hooks the new Explorer automatically. To update,
do the same with the new release; it replaces the installed copy.

## Uninstall

Uninstall it from **Settings > Apps > Installed apps** like any other app. This removes the hooks
from the running Explorer, the sign-in entry, and the installed files.

## Commands

Run these from the install folder (`%LOCALAPPDATA%\Programs\InstantAltTab`) or the unzipped folder;
both act on the installed copy if there is one.

| Command | |
|---|---|
| `install` | Install for the current user; starts now and at sign-in |
| `uninstall` | Remove the hooks from Explorer and delete the installed files |
| `status` | Show whether it is installed, running, and active in Explorer |
| `start` | Start the agent (the one next to it if not installed) |
| `stop` | Stop the agent and restore stock Alt+Tab until the next start or sign-in |

## Compatibility

Explorer's internals change with nearly every Windows update, so nothing about them is written into
Instant Alt+Tab. Each time it starts, it works out what it needs from the exact Explorer files on
your machine: one of the two DLLs involved by Microsoft's public symbol names, the other by what its
code does ([details](docs/internals.md#finding-the-internals)). Updates that only move code around
are handled without a new release.

If an update changes Alt+Tab in a way it doesn't recognize, it leaves Alt+Tab alone and shows a
message once; `status` says why. It was developed and tested on Windows 11 25H2, and its
discovery finds everything in three consecutive monthly builds of the two DLLs.

## Safety

- **Nothing is hooked unless everything is found.** Every value must be found exactly once, the
  hooked code in memory must be the code that was analysed, and the hooks that depend on the
  layout of Explorer's objects go in only after the first Alt+Tab has confirmed that layout against
  what Windows actually passes around.
- **It turns itself off when in doubt.** If Windows does anything unexpected (a failed call, a card
  list that isn't the one the menu shows, an exception), it drops the kept menu, Alt+Tab stays
  stock until Explorer restarts, and a message tells you. `status` shows it too.
- **It stops for good after repeated crashes.** If Explorer crashes twice within an hour while the
  hooks are active, the agent stops loading them until you install again.
- **`stop` and `uninstall` remove every hook from the running Explorer.** If a hooked call is still
  running, the DLL stays loaded but inactive until Explorer restarts.

Some antivirus software flags programs that load code into other processes. The full source is here
if you want to check what it does or build it yourself.

## Footprint

- **Disk:** about 1.5 MB installed, plus a few KB of looked-up symbol names per Windows build.
- **Network:** once per Windows update, the agent downloads twinui.pcshell.dll's public symbols
  (about 30 MB) from Microsoft's symbol server, keeps a dozen names from it and deletes it. Nothing
  else is downloaded or sent.
- **Explorer:** keeps one Alt+Tab menu alive instead of building one per press; it is the same menu
  Windows builds every time you press Alt+Tab. Over a 30-opening test with windows opening and
  closing, Explorer's memory and handle count stayed flat.
- **Background:** the agent sleeps until Explorer starts or exits. Working out the internals takes
  about 0.2 s per Explorer start once the symbols are known.

## Building

Requires Visual Studio 2022 17.5 or later with the "Desktop development with C++" workload, which
includes CMake and the Windows SDK.

```
cmake --preset default
cmake --build --preset release
```

The three binaries end up in `build\bin\Release`. To try them without installing, run
`instant-alt-tab start` there (with no installed copy, since commands prefer that one).
MinHook and Zydis are downloaded during configuration. `--target discover` builds a tool that runs
the discovery on any copies of the two DLLs ([docs](docs/internals.md#checking-a-windows-build)).

## Troubleshooting

`instant-alt-tab status` shows the current state, including why Alt+Tab is stock if it is. The
agent writes `agent.log` and the hooks write `hook.log` to the install folder; both record starts,
stops, menu rebuilds and anything unexpected.

## License

[MIT](LICENSE)
