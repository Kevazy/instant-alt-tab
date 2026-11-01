#pragma once

// Finds everything the hooks need in a given pair of DLL files, from first
// principles: twinui.pcshell.dll by its public symbol names, the switcher DLL
// (which has no public symbols) by what its code does. Every rule must match
// exactly once; anything ambiguous or missing is an error, and the hooks then
// stay off.

#include "../shared/AltTabInternals.h"

#include <windows.h>

#include <optional>
#include <string>

namespace Discovery {

struct Result {
    std::optional<AltTab::Internals> internals;
    std::wstring error;       // what didn't match, when internals is empty
    bool retryLater = false;  // the symbol download failed; the same files may work later
};

// `cacheDirectory` (with a trailing backslash) keeps the looked-up symbol names.
// `cancel`, if set, stops a symbol download in progress.
Result Discover(const std::wstring& twinuiPath, const std::wstring& switcherPath, const std::wstring& cacheDirectory,
                HANDLE cancel = nullptr);

}  // namespace Discovery
