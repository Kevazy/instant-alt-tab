// twinui.pcshell.dll: functions by their public symbol names; member offsets
// and vtable slots from what those functions do with their parameters.

#include "FindInternals.h"
#include "Symbols.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace Discovery {

namespace {

const std::wstring kSwitcherApi = L"WindowsInternal::ComposableShell::Experiences::Switcher::";
const std::wstring kHostWindowClass =
    L"winrt::Windows::Internal::Shell::XamlExplorerHost::implementation::XamlExplorerHostWindow";
const std::wstring kHostWindowInterface = L"winrt::Windows::Internal::Shell::XamlExplorerHost::IXamlExplorerHostWindow";

const std::wstring kCreateControl = L"XamlAltTabViewHost::CreateAltTabControl";
const std::wstring kShow = L"XamlAltTabViewHost::Show";
const std::wstring kDismiss = L"XamlAltTabViewHost::v_DismissView";
const std::wstring kDisplay = L"XamlAltTabViewHost::DisplayAltTab";
const std::wstring kGetItemForView = L"CachedDataSource::GetItemForView";
const std::wstring kRegisterRoot = kSwitcherApi + L"__IHostInformationRegistrarPublicNonVirtuals::RegisterXamlRoot";
// twinui's calls into the AltTab control's interface.
const std::wstring kNavigateToItem = kSwitcherApi + L"__IAltTabPublicNonVirtuals::NavigateToItem";
const std::wstring kNavigate = kSwitcherApi + L"__IAltTabPublicNonVirtuals::Navigate";
// Converts the host's data source for the card list; the factory passes it the member's address.
const std::wstring kDataSourceToCx = L"to_cx<Windows::Internal::ComposableShell::Multitasking::ISwitchItemDataSource>";
// The host window's XamlContent property: twinui's caller, and the window's own accessors.
const std::wstring kContentProperty =
    L"winrt::impl::consume_Windows_Internal_Shell_XamlExplorerHost_IXamlExplorerHostWindow<" + kHostWindowInterface +
    L">::XamlContent";
const std::wstring kContentGetter =
    L"winrt::impl::produce<" + kHostWindowClass + L"," + kHostWindowInterface + L">::get_XamlContent";
const std::wstring kContentSetter =
    L"winrt::impl::produce<" + kHostWindowClass + L"," + kHostWindowInterface + L">::put_XamlContent";
// The interface the host's data source member must have.
const std::wstring kDataSourceId = L"_uuidof_?AUISwitchItemDataSourceMultitaskingComposableShellInternalWindows";
const std::wstring kSwitchItemId =
    L"winrt::impl::guid_v<winrt::Windows::Internal::ComposableShell::Multitasking::ISwitchItem>";
// The class of the shell's items: one vtable per interface it implements.
const std::wstring kSwitchItemVtable = L"SwitchItem::`vftable'";

class Names {
public:
    Names(const PeFile& file, const std::wstring& cacheDirectory, HANDLE cancel) {
        std::vector<std::wstring> names{
            kCreateControl,  kShow,           kDismiss,      kDisplay,        kRegisterRoot,
            kGetItemForView, kNavigateToItem, kNavigate,     kDataSourceToCx, kContentProperty,
            kContentGetter,  kContentSetter,  kSwitchItemId, kDataSourceId,   kSwitchItemVtable};
        Symbols symbols = LookUpSymbols(file, names, cacheDirectory, cancel);
        if (!symbols.error.empty()) throw RuleFailed{symbols.error, symbols.retryLater, symbols.notPublished};
        m_rvas = std::move(symbols.rvas);
    }
    std::set<uint32_t> All(const std::wstring& name) const {
        auto found = m_rvas.find(name);
        return found == m_rvas.end() ? std::set<uint32_t>{} : std::set(found->second.begin(), found->second.end());
    }
    uint32_t One(const std::wstring& name) const { return ExactlyOne(name.c_str(), All(name)); }

private:
    std::map<std::wstring, std::vector<uint32_t>> m_rvas;
};

// What a call's first argument is, at every call from `steps` to `target`.
std::set<Value> FirstArguments(const std::vector<Step>& steps, uint32_t target) {
    std::set<Value> found;
    for (const Step& step : steps) {
        if (step.call && step.target == target) found.insert(step.Argument(0));
    }
    return found;
}

// The offset X when every first argument is "parameter 0 + X" (`loads` 0) or "the qword at parameter 0 + X" (1).
uint32_t MemberOffset(const wchar_t* what, const std::set<Value>& arguments, int loads) {
    std::set<uint32_t> offsets;
    for (const Value& value : arguments) {
        bool member = value.kind == Value::Kind::Pointer && value.parameter == 0 && value.loads == loads &&
                      (loads == 0 ? value.number > 0 : value.number == 0);
        offsets.insert(member ? static_cast<uint32_t>(loads == 0 ? value.number : value.offsets[0]) : 0);
    }
    if (offsets.contains(0)) throw RuleFailed{std::wstring(what) + L": a call's argument isn't a member"};
    return ExactlyOne(what, offsets);
}

GUID InterfaceId(const PeFile& file, uint32_t rva, const wchar_t* what) {
    auto bytes = file.Read(rva, sizeof(GUID));
    if (bytes.empty()) throw RuleFailed{std::wstring(what) + L"'s interface ID isn't in the file"};
    GUID id;
    std::memcpy(&id, bytes.data(), sizeof(GUID));
    return id;
}

// Whether `rva` is a vtable: data whose first entry (QueryInterface, for a COM class) is code.
bool IsVtable(const PeFile& file, uint32_t rva) {
    auto entry = file.Read(rva, sizeof(uint64_t));
    if (file.IsCode(rva) || entry.empty()) return false;
    uint64_t address;
    std::memcpy(&address, entry.data(), sizeof(address));
    return address > file.ImageBase() && file.IsCode(static_cast<uint32_t>(address - file.ImageBase()));
}

uint32_t SlotCalledBy(const PeFile& file, uint32_t wrapper, int depth, const wchar_t* what) {
    auto slot = VtableSlotCalled(WalkFunction(file, wrapper), depth);
    if (!slot) throw RuleFailed{std::wstring(what) + L": no single vtable call"};
    return *slot;
}

}  // namespace

void FindTwinuiInternals(const PeFile& file, const std::wstring& cacheDirectory, HANDLE cancel,
                         AltTab::TwinuiInternals& twinui) {
    Names names(file, cacheDirectory, cancel);
    twinui.createControl = FunctionAt(file, names.One(kCreateControl), L"CreateAltTabControl");
    twinui.show = FunctionAt(file, names.One(kShow), L"Show");
    twinui.dismiss = FunctionAt(file, names.One(kDismiss), L"v_DismissView");
    twinui.display = FunctionAt(file, names.One(kDisplay), L"DisplayAltTab");
    twinui.registerRoot = FunctionAt(file, names.One(kRegisterRoot), L"RegisterXamlRoot");
    std::vector<Step> show = WalkFunction(file, twinui.show.rva);

    // RegisterXamlRoot shares its code with unrelated functions (the linker
    // folds identical functions), so only Show's call to it is the real one.
    std::set<uint32_t> returns;
    for (const Step& step : show) {
        if (step.call && step.target == twinui.registerRoot.rva) returns.insert(step.rva + step.length);
    }
    twinui.registerRootReturnInShow = ExactlyOne(L"Show's call to RegisterXamlRoot", returns);

    // The content property's setter is the accessor Show calls, with the host window member's address.
    std::set<uint32_t> setters;
    for (uint32_t accessor : names.All(kContentProperty)) {
        if (!FirstArguments(show, accessor).empty()) setters.insert(accessor);
    }
    twinui.setHostContent = FunctionAt(file, ExactlyOne(L"XamlContent setter called by Show", setters), L"setter");
    twinui.hostWindow = MemberOffset(L"host window member", FirstArguments(show, twinui.setHostContent.rva), 0);
    // The setter's `this` points to the window's interface pointer, hence two loads to its vtable.
    twinui.hostContentSetterSlot = SlotCalledBy(file, twinui.setHostContent.rva, 2, L"XamlContent setter slot");
    // A WinRT property's getter always directly precedes its setter in the vtable.
    twinui.hostContentGetterSlot = twinui.hostContentSetterSlot - 1;
    twinui.hostContentGetter = FunctionAt(file, names.One(kContentGetter), L"get_XamlContent").rva;
    twinui.hostContentSetter = FunctionAt(file, names.One(kContentSetter), L"put_XamlContent").rva;

    // The control member is what Show calls NavigateToItem and Navigate on;
    // those wrappers take the interface pointer itself, one load from the vtable.
    uint32_t navigateToItem = names.One(kNavigateToItem), navigate = names.One(kNavigate);
    std::set<Value> controls = FirstArguments(show, navigateToItem);
    controls.merge(FirstArguments(show, navigate));
    twinui.hostControl = MemberOffset(L"control member", controls, 1);
    twinui.navigateToItemSlot = SlotCalledBy(file, navigateToItem, 1, L"NavigateToItem slot");
    twinui.navigateSlot = SlotCalledBy(file, navigate, 1, L"Navigate slot");

    std::vector<Step> factory = WalkFunction(file, twinui.createControl.rva);
    twinui.hostDataSource = MemberOffset(L"data source member", FirstArguments(factory, names.One(kDataSourceToCx)), 0);

    twinui.switchItem = InterfaceId(file, names.One(kSwitchItemId), L"ISwitchItem");
    twinui.switchItemDataSource = InterfaceId(file, names.One(kDataSourceId), L"ISwitchItemDataSource");

    // Optional: without them the hook learns where cards keep their item from
    // the first opening that selects a window, one opening later.
    std::set<uint32_t> vtables = names.All(kSwitchItemVtable);
    bool usable = vtables.size() <= std::size(twinui.switchItemVtables) &&
                  std::all_of(vtables.begin(), vtables.end(), [&](uint32_t rva) { return IsVtable(file, rva); });
    std::fill(std::begin(twinui.switchItemVtables), std::end(twinui.switchItemVtables), 0);
    if (usable) std::copy(vtables.begin(), vtables.end(), twinui.switchItemVtables);

    // Show asks for the foreground window's item after storing the control and
    // before positioning the selection; the hooks check the control there.
    twinui.getItemForView = FunctionAt(file, names.One(kGetItemForView), L"GetItemForView");
    if (FirstArguments(show, twinui.getItemForView.rva).empty()) throw RuleFailed{L"Show doesn't call GetItemForView"};
}

}  // namespace Discovery
