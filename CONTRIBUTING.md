# Contributing

[docs/internals.md](docs/internals.md) explains how everything works and the words used for it;
read its [glossary](docs/internals.md#words-used-here) first if terms like vtable, RVA or PDB are
new. The README's [Building](README.md#building) section covers the build.

## Code style

Formatting is whatever `clang-format` makes of it with the repository's `.clang-format`; run it on
the files you change.

Names follow Microsoft's C++ style, the one used in Windows Terminal and PowerToys:

| What | Style | Example |
|---|---|---|
| Source files | PascalCase, after the type or module they hold | `MenuState.h` |
| An executable's entry point | `main.cpp`; the DLL's is `DllMain.cpp` | |
| Folders | lowercase | `src/discovery` |
| Namespaces, types, functions | PascalCase | `Discovery::Discover`, `class CrashGuard` |
| Each part of the program | a namespace or class named after it; small helpers used everywhere are plain functions | `Discovery::Discover` |
| Locals, parameters, struct fields | camelCase | `menuCount` |
| Class members | `m_` + camelCase; static ones `s_` | `m_handle` |
| Globals and thread-locals | `g_` and `t_` + camelCase | `g_menuCount` |
| Constants, and values of plain enums | `k` + PascalCase | `kTimeoutMs` |
| Values of `enum class` | PascalCase | `Reason::Timeout` |

Names that exist outside the source don't follow this, and must not change: the executable and DLL
names (lowercase with hyphens, like most command-line tools), the files the agent writes, the named
mutexes and events, and the DLL's exports. Installed copies and running agents rely on them.

Comments say why, in plain words, next to the code they explain. The docs describe ideas, not
where things are, so they only need changing when an idea changes.

## Working on it

- **There are no automated tests**, and CI only builds. A change is checked by running it: `discover`
  on as many Windows builds as you can get, the checklist in the docs'
  [Checking a Windows build](docs/internals.md#checking-a-windows-build), and `measure-latency`.
- **Run `instant-alt-tab stop` before rebuilding**: Explorer keeps the hook DLL loaded, so the build
  can't replace it. If the DLL stays loaded anyway, restart Explorer.
- **A bug in the hooks can crash Explorer.** After two crashes within an hour the agent stops loading
  anything, and `crash-guard.txt` next to it (in the build folder too) remembers that; delete it to
  try again.
- **`hook.log` and `agent.log`** next to the agent say what happened, including every rebuild and
  why the hooks turned off.
- **Releases**: pushing a tag like `v1.2.0` builds and publishes one; the version itself is set in
  `CMakeLists.txt`. The `debug` build preset exists for stepping through the code.

## Changing what discovery finds

- **The agent and the hook DLL share one struct** of everything discovery found, copied into
  Explorer as is. Its layout is their contract: raise its version whenever it changes.
- **A rule must match exactly once.** Anything else is an error, and the hooks stay off. Check what
  the rule found for consistency too, and describe the rule in a comment next to it.
- **A function that gets hooked** is recorded with its first bytes, which are checked in memory
  before hooking; other values are plain offsets.
- **`discover` prints every value**, so add new ones to its output.
- **Check at run time** what a wrong value could do harm with, and turn the hooks off on a mismatch
  (see "Safety" in the docs).
- **Something optional** (like the data model DLL's item class) must never stop the hooks: the
  struct stays zero when it isn't found, and the hooks work without it.
- **Run `discover` on as many builds as you can get**; the docs'
  [Checking a Windows build](docs/internals.md#checking-a-windows-build) says where to find them.
