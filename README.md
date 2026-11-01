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

After Explorer starts, the first two Alt+Tabs open at the usual speed: the first loads Windows'
Alt+Tab code, which the hooks attach to, and the second builds the menu that is kept from then on.
[docs/internals.md](docs/internals.md) explains each hook and how the internals were found.

## Install

Requires Windows 11 with the exact Explorer components it was built for (see
[Compatibility](#compatibility)). No administrator rights.

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

Explorer's internals change between Windows builds, so the hooks are tied to exact versions of two
system files:

| File | Version |
|---|---|
| `twinui.pcshell.dll` | 10.0.26100.8524 |
| `WindowsInternal.ComposableShell.Experiences.Switcher.dll` | 10.0.26100.8972 |

These shipped with Windows 11 25H2 (build 26200.9168). On any other version Instant Alt+Tab does
nothing: Alt+Tab stays stock and `hook.log` says why. Supporting a new build means re-deriving the
offsets with [`tools/find_offsets.py`](tools/find_offsets.py); see
[docs/internals.md](docs/internals.md#supporting-another-windows-build).

## Safety

- **Nothing is hooked unless everything matches.** Both modules must be the exact versions above,
  and every hooked function's first bytes must match.
- **It turns itself off when in doubt.** If Windows does anything unexpected (a failed call, a card
  list that isn't the one the menu shows, an exception), it drops the kept menu and Alt+Tab stays
  stock until Explorer restarts. `status` shows when that happened.
- **It stops for good after repeated crashes.** If Explorer crashes twice within an hour while the
  hooks are active, the agent stops loading them until you install again.
- **`stop` and `uninstall` remove every hook from the running Explorer.** If a hooked call is still
  running, the DLL stays loaded but inactive until Explorer restarts.

Some antivirus software flags programs that load code into other processes. The full source is here
if you want to check what it does or build it yourself.

## Building

Requires Visual Studio 2022 17.5 or later with the "Desktop development with C++" workload, which
includes CMake and the Windows SDK.

```
cmake --preset default
cmake --build --preset release
```

The three binaries end up in `build\bin\Release`. To try them without installing, run
`instant-alt-tab start` there (with no installed copy, since commands prefer that one).
MinHook is downloaded during configuration.

## Troubleshooting

`instant-alt-tab status` shows the current state. The agent writes `agent.log` and the hooks write
`hook.log` to the install folder; both record starts, stops, menu rebuilds and anything unexpected.

## License

[MIT](LICENSE)
