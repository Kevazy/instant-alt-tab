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

void FindDataModelInternals(const PeFile& file, const std::wstring& cacheDirectory, HANDLE cancel,
                            AltTab::DataModelInternals& dataModel) {
    dataModel = {};
    Symbols symbols = LookUpSymbols(file, {kItemVtable}, cacheDirectory, cancel);
    if (!symbols.error.empty()) return;
    const std::vector<uint32_t>& found = symbols.rvas[kItemVtable];
    std::set<uint32_t> vtables(found.begin(), found.end());
    if (vtables.empty() || vtables.size() > std::size(dataModel.itemVtables)) return;
    if (!std::all_of(vtables.begin(), vtables.end(), [&](uint32_t rva) { return IsVtable(file, rva); })) return;
    std::copy(vtables.begin(), vtables.end(), dataModel.itemVtables);
    dataModel.module = {file.TimeDateStamp(), file.SizeOfImage()};
}

}  // namespace Discovery
