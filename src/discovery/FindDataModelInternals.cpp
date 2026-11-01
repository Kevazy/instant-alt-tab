// SwitcherDataModel.dll: the vtables of the class of the shell's items, by
// their public symbol name. Optional: anything missing leaves them zero, and
// discovery succeeds anyway.

#include "FindInternals.h"
#include "Symbols.h"

#include <algorithm>
#include <iterator>
#include <set>

namespace Discovery {

namespace {

// One vtable per interface the class implements.
const std::wstring kItemVtable = L"RootSwitchItem::`vftable'";

}  // namespace

std::wstring FindDataModelInternals(const PeFile& file, const std::wstring& cacheDirectory, HANDLE cancel,
                                    AltTab::DataModelInternals& dataModel, bool& downloadFailed) {
    dataModel = {};
    downloadFailed = false;
    if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return L"stopped";
    Symbols symbols = LookUpSymbols(file, {kItemVtable}, cacheDirectory, cancel);
    if (!symbols.error.empty()) {
        downloadFailed = symbols.retryLater;
        return symbols.error;
    }
    const std::vector<uint32_t>& found = symbols.rvas[kItemVtable];
    std::set<uint32_t> vtables(found.begin(), found.end());
    if (vtables.empty()) return L"its symbols don't name the item class";
    if (vtables.size() > std::size(dataModel.itemVtables)) return L"the item class has more vtables than expected";
    if (!std::all_of(vtables.begin(), vtables.end(), [&](uint32_t rva) { return IsVtable(file, rva); })) {
        return L"the item class's vtables aren't vtables";
    }
    std::copy(vtables.begin(), vtables.end(), dataModel.itemVtables);
    dataModel.module = {file.TimeDateStamp(), file.SizeOfImage()};
    return {};
}

}  // namespace Discovery
