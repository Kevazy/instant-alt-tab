#include "discovery.h"

#include "rules.h"

#include <cstring>

namespace discovery {

internals::Function FunctionAt(const PeFile& file, uint32_t rva, const wchar_t* what) {
    internals::Function function{rva};
    auto entry = file.Read(rva, sizeof(function.entry));
    if (!file.IsCode(rva) || entry.empty()) throw RuleFailed{std::wstring(what) + L" isn't code"};
    std::memcpy(function.entry, entry.data(), sizeof(function.entry));
    return function;
}

Result Discover(const std::wstring& twinuiPath, const std::wstring& switcherPath, const std::wstring& cacheDirectory) {
    Result result;
    auto twinui = PeFile::Load(twinuiPath);
    auto switcher = PeFile::Load(switcherPath);
    if (!twinui || !switcher) {
        result.error = L"couldn't read " + (twinui ? switcherPath : twinuiPath);
        return result;
    }
    internals::Internals found;
    try {
        FindTwinui(*twinui, cacheDirectory, found.twinui);
        FindSwitcher(*switcher, found.switcher);
    } catch (const RuleFailed& failure) {
        result.error = failure.what;
        result.retryLater = failure.retryLater;
        return result;
    }
    found.twinui.module = {twinui->TimeDateStamp(), twinui->SizeOfImage()};
    found.switcher.module = {switcher->TimeDateStamp(), switcher->SizeOfImage()};
    result.internals = found;
    return result;
}

}  // namespace discovery
