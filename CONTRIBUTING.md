# Contributing

[docs/internals.md](docs/internals.md) explains how everything works, where each part of the source
lives, and the words used for it. The README's [Building](README.md#building) section covers the build.

## Code style

Formatting is whatever `clang-format` makes of it with the repository's `.clang-format`; run it on
the files you change.

Names follow Microsoft's C++ style, the one used in Windows Terminal and PowerToys:

| What | Style | Example |
|---|---|---|
| Source files | PascalCase, after the type or module they hold | `PersistentMenu.cpp`, `ExplorerProcess.h` |
| An executable's entry point | `main.cpp`; the DLL's is `DllMain.cpp` | |
| Folders | lowercase | `src/discovery` |
| Namespaces, types, functions | PascalCase | `Discovery::Discover`, `class CrashGuard` |
| Free functions of a module | in a namespace named after its file | `PersistentMenu::InstallHooks` |
| Locals, parameters, struct fields | camelCase | `hostControl` |
| Class members | `m_` + camelCase; static ones `s_` | `m_process` |
| Globals and thread-locals | `g_` and `t_` + camelCase | `g_internals` |
| Constants and enumerators | `k` + PascalCase | `kAgentMutex` |

Names that exist outside the source don't follow this, and must not change: the executable and DLL
names (lowercase with hyphens, like most command-line tools), the files the agent writes, the named
mutexes and events, and the DLL's exports. Installed copies and running agents rely on them.

Comments say why, in plain words. A newcomer should be able to follow the code with
[docs/internals.md](docs/internals.md) at hand.

## Adding a value discovery must find

1. Add the field to `src/shared/AltTabInternals.h` and raise `kInternalsVersion`: the agent copies
   that struct into Explorer as is, so its layout is the contract with the hook DLL.
2. Write the rule in `src/discovery/TwinuiRules.cpp` or `SwitcherRules.cpp`. A rule must match
   exactly once (`ExactlyOne`); anything else is an error, and the hooks stay off.
3. Print it in `tools/Discover.cpp`.
4. Use it in the hook through `g_internals`, and check it at run time where a wrong value could do
   harm (see "Safety" in the docs).
5. Describe the rule in the "Finding the internals" table of `docs/internals.md`.
6. Run `discover` on every build of the two DLLs you have; the docs'
   [Checking a Windows build](docs/internals.md#checking-a-windows-build) says where to find them.
