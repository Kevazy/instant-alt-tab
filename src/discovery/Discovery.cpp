#include "Discovery.h"

#include "FindInternals.h"

#include <cstring>

namespace Discovery {

AltTab::FunctionLocation FunctionAt(const PeFile& file, uint32_t rva, const wchar_t* what) {
    AltTab::FunctionLocation function{rva};
    auto entry = file.Read(rva, sizeof(function.entry));
    if (!file.IsCode(rva) || entry.empty()) throw RuleFailed{std::wstring(what) + L" isn't code"};
    std::memcpy(function.entry, entry.data(), sizeof(function.entry));
    return function;
}

bool IsVtable(const PeFile& file, uint32_t rva) {
    auto entry = file.Read(rva, sizeof(uint64_t));
    if (file.IsCode(rva) || entry.empty()) return false;
    uint64_t address;
    std::memcpy(&address, entry.data(), sizeof(address));
    return address > file.ImageBase() && file.IsCode(static_cast<uint32_t>(address - file.ImageBase()));
}

Result Discover(const std::wstring& twinuiPath, const std::wstring& switcherPath, const std::wstring& dataModelPath,
                const std::wstring& cacheDirectory, HANDLE cancel) {
    Result result;
    auto twinui = PeFile::Load(twinuiPath);
    auto switcher = PeFile::Load(switcherPath);
    if (!twinui || !switcher) {
        result.error = L"couldn't read " + (twinui ? switcherPath : twinuiPath);
        return result;
    }
    AltTab::Internals found;
    try {
        FindTwinuiInternals(*twinui, cacheDirectory, cancel, found.twinui);
        FindSwitcherInternals(*switcher, found.switcher);
    } catch (const RuleFailed& failure) {
        result.error = failure.what;
        result.retryLater = failure.retryLater;
        result.notPublished = failure.notPublished;
        return result;
    }
    found.twinui.module = {twinui->TimeDateStamp(), twinui->SizeOfImage()};
    found.switcher.module = {switcher->TimeDateStamp(), switcher->SizeOfImage()};
    if (auto dataModel = dataModelPath.empty() ? std::nullopt : PeFile::Load(dataModelPath)) {
        FindDataModelInternals(*dataModel, cacheDirectory, cancel, found.dataModel);
    }
    result.internals = found;
    return result;
}

}  // namespace Discovery
