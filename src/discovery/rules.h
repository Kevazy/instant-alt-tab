#pragma once

// The rules for each DLL, and what they share. A rule that doesn't match
// exactly once throws RuleFailed, naming what it looked for.

#include "code.h"
#include "../shared/internals.h"

#include <set>
#include <string>

namespace discovery {

struct RuleFailed {
    std::wstring what;
    bool retryLater = false;
};

template <typename T>
T Unique(const wchar_t* what, const std::set<T>& found) {
    if (found.size() != 1) {
        throw RuleFailed{std::wstring(what) + L": expected one match, found " + std::to_wstring(found.size())};
    }
    return *found.begin();
}

// A function and its entry bytes, which must be code in the file.
internals::Function FunctionAt(const PeFile& file, uint32_t rva, const wchar_t* what);

void FindTwinui(const PeFile& file, const std::wstring& cacheDirectory, internals::Twinui& twinui);
void FindSwitcher(const PeFile& file, internals::Switcher& switcher);

}  // namespace discovery
