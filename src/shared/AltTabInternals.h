#pragma once

// What the agent found out about the running Windows build, handed to the hook
// DLL when it starts: where the hooked functions are and the layout of the
// objects they touch. All of it is derived on the user's machine from the DLLs
// Explorer has loaded (src/discovery); none of it is written down for a
// particular Windows version.

#include <windows.h>

#include <cstdint>

namespace AltTab {

inline constexpr wchar_t kTwinuiDll[] = L"twinui.pcshell.dll";
inline constexpr wchar_t kSwitcherDll[] = L"WindowsInternal.ComposableShell.Experiences.Switcher.dll";

// The interface ID of an element type the Alt+Tab menu never contains. After
// every card change the switcher searches each card for it; the hook answers
// that search for the kept menu's cards, and discovery finds the search by
// this ID. Interface IDs are contracts: COM never changes the ID of an
// interface, a changed interface gets a new one.
inline constexpr GUID kElementTypeNotInMenu{
    0xae506dd8, 0xf75c, 0x3cce, {0xbe, 0x60, 0xd9, 0x82, 0x7a, 0xb2, 0x44, 0x4e}};

// Which build of a DLL this is: two fields of its PE header, which together
// also name the file on Microsoft's symbol server.
struct ImageIdentity {
    uint32_t timeDateStamp;
    uint32_t sizeOfImage;

    bool operator==(const ImageIdentity&) const = default;
};

// A function in a DLL: its offset from the DLL's load address (its relative
// virtual address, RVA) and its first bytes in the analysed file, which the
// hook compares with memory before hooking it.
struct FunctionLocation {
    uint32_t rva;
    uint8_t entry[16];
};

// twinui.pcshell.dll hosts the Alt+Tab view.
struct TwinuiInternals {
    ImageIdentity module;
    FunctionLocation createControl;     // XamlAltTabViewHost::CreateAltTabControl(host) -> control
    FunctionLocation show;              // XamlAltTabViewHost::Show(host, display, flags, view)
    FunctionLocation dismiss;           // XamlAltTabViewHost::v_DismissView(host)
    FunctionLocation display;           // XamlAltTabViewHost::DisplayAltTab(host)
    FunctionLocation setHostContent;    // sets the host window's XAML content: (window**, content**)
    FunctionLocation registerRoot;      // RegisterXamlRoot(registrar, root), folded with unrelated functions
    FunctionLocation getItemForView;    // CachedDataSource::GetItemForView: Show calls it after storing the control
    uint32_t registerRootReturnInShow;  // return address of Show's call to it: the only call that registers a root
    uint32_t hostContentGetter, hostContentSetter;          // the host window's own get/put XamlContent
    uint32_t hostContentGetterSlot, hostContentSetterSlot;  // and their vtable slots
    uint32_t hostDataSource, hostControl, hostWindow;       // XamlAltTabViewHost members
    uint32_t navigateToItemSlot, navigateSlot;              // the AltTab control's NavigateToItem and Navigate
    GUID switchItem;                                        // ISwitchItem: the shell's item behind each card
    GUID switchItemDataSource;                              // ISwitchItemDataSource: what the host's data source is
};

// The switcher DLL is the Alt+Tab UI: the AltTab control and its card list.
struct SwitcherInternals {
    ImageIdentity module;
    FunctionLocation syncList;        // re-syncs a card list with the window list (list)
    FunctionLocation sortList;        // sorts a card list in place, one replacement per move (list)
    FunctionLocation closeList;       // runs when a card is chosen: unsubscribes the list from the window list (list)
    FunctionLocation findDescendant;  // recursive search for an element of kElementTypeNotInMenu (root)
    FunctionLocation vectorRemoveAt;  // the card vector's RemoveAt(vector, index)
    FunctionLocation vectorInsertAt;  // InsertAt(vector, index, card)
    FunctionLocation vectorNotify;    // and its change notifier(vector, change, index)
    uint32_t listCards;               // card list member: the card vector
    uint32_t listClosed;              // card list member: the byte closeList sets
    uint32_t vectorStorage;           // card vector member: its begin/end pointer pair
    uint32_t vectorObserved;          // card vector member: notifications are raised only while this byte is set
};

// Identify the struct below. The version is its layout's, not the program's:
// raise it whenever a field is added, removed or moved.
inline constexpr uint32_t kInternalsMagic = 0x42415441;  // "ATAB"
inline constexpr uint32_t kInternalsVersion = 2;

// Everything above, passed to the hook DLL's Start as its parameter. The agent
// copies it into Explorer's memory byte for byte, so its layout is the contract
// between the two.
struct Internals {
    uint32_t magic = kInternalsMagic;
    uint32_t version = kInternalsVersion;
    uint32_t size = sizeof(Internals);
    TwinuiInternals twinui{};
    SwitcherInternals switcher{};
};

}  // namespace AltTab
