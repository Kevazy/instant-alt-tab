#pragma once

// The rules for each DLL, and what they share. A rule that doesn't match
// exactly once throws RuleFailed, naming what it looked for.

#include "CodeAnalysis.h"
#include "../shared/AltTabInternals.h"

#include <set>
#include <string>

namespace Discovery {

struct RuleFailed {
    std::wstring what;
    bool retryLater = false;    // the symbol download failed
    bool notPublished = false;  // because Microsoft hasn't published the symbols
};

template <typename T>
T ExactlyOne(const wchar_t* what, const std::set<T>& found) {
    if (found.size() != 1) {
        throw RuleFailed{std::wstring(what) + L": expected one match, found " + std::to_wstring(found.size())};
    }
    return *found.begin();
}

// A function and its entry bytes, which must be code in the file.
AltTab::FunctionLocation FunctionAt(const PeFile& file, uint32_t rva, const wchar_t* what);

void FindTwinuiInternals(const PeFile& file, const std::wstring& cacheDirectory, HANDLE cancel,
                         AltTab::TwinuiInternals& twinui);
void FindSwitcherInternals(const PeFile& file, AltTab::SwitcherInternals& switcher);
// Optional: never throws; leaves `dataModel` zero when anything is missing.
void FindDataModelInternals(const PeFile& file, const std::wstring& cacheDirectory, HANDLE cancel,
                            AltTab::DataModelInternals& dataModel);

// Whether `rva` is a vtable: data whose first entry (QueryInterface, for a COM class) is code.
bool IsVtable(const PeFile& file, uint32_t rva);

}  // namespace Discovery
