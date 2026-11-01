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

// The DLL files to analyse. `dataModel` may be empty: what it gives is optional.
struct DllPaths {
    std::wstring twinui, switcher, dataModel;
};

struct Result {
    std::optional<AltTab::Internals> internals;
    std::wstring error;                    // what didn't match, when internals is empty
    bool retryLater = false;               // the symbol download failed; the same files may work later
    bool notPublished = false;             // because Microsoft hasn't published the symbols (yet)
    std::wstring dataModelError;           // why the optional data model values are missing, if they are
    bool dataModelDownloadFailed = false;  // because their symbol download failed
};

// `cacheDirectory` (with a trailing backslash) keeps the looked-up symbol names.
// `cancel`, if set, stops a symbol download in progress.
Result Discover(const DllPaths& dlls, const std::wstring& cacheDirectory, HANDLE cancel = nullptr);

}  // namespace Discovery
