#pragma once

// What the agent found out about the running Windows build, handed to the hook
// DLL when it starts: where the hooked functions are and the layout of the
// objects they touch. All of it is derived on the user's machine from the DLLs
// Explorer has loaded (src/discovery); none of it is written down for a
// particular Windows version.

#include <windows.h>

#include <cstdint>

namespace internals {

inline constexpr wchar_t kTwinuiDll[] = L"twinui.pcshell.dll";
inline constexpr wchar_t kSwitcherDll[] = L"WindowsInternal.ComposableShell.Experiences.Switcher.dll";

// Interface IDs are contracts: COM never changes the ID of an interface, a
// changed interface gets a new one. The element type the switcher's
// descendant search looks for; discovery also uses it to find that search.
inline constexpr GUID kSearchedType{0xae506dd8, 0xf75c, 0x3cce, {0xbe, 0x60, 0xd9, 0x82, 0x7a, 0xb2, 0x44, 0x4e}};

struct Module {
    uint32_t timeDateStamp;
    uint32_t sizeOfImage;

    bool operator==(const Module&) const = default;
};

struct Function {
    uint32_t rva;
    uint8_t entry[16];  // its first bytes in the analysed file; the hook compares them with memory
};

// twinui.pcshell.dll hosts the Alt+Tab view.
struct Twinui {
    Module module;
    Function createControl;             // XamlAltTabViewHost::CreateAltTabControl(host) -> control
    Function show;                      // XamlAltTabViewHost::Show(host, display, flags, view)
    Function dismiss;                   // XamlAltTabViewHost::v_DismissView(host)
    Function display;                   // XamlAltTabViewHost::DisplayAltTab(host)
    Function setHostContent;            // sets the host window's XAML content: (window**, content**)
    Function registerRoot;              // RegisterXamlRoot(registrar, root), folded with unrelated functions
    uint32_t registerRootReturnInShow;  // return address of Show's call to it: the only call that registers a root
    uint32_t hostContentGetter, hostContentSetter;          // the host window's own get/put XamlContent
    uint32_t hostContentGetterSlot, hostContentSetterSlot;  // and their vtable slots
    uint32_t hostDataSource, hostControl, hostWindow;       // XamlAltTabViewHost members
    uint32_t navigateToItemSlot, navigateSlot;              // the AltTab control's NavigateToItem and Navigate
    GUID switchItem;                                        // ISwitchItem: the shell's item behind each card
};

// The switcher DLL is the Alt+Tab UI: the AltTab control and its card list.
struct Switcher {
    Module module;
    Function syncList;        // re-syncs a card list with the window list (list)
    Function sortList;        // sorts a card list in place, one replacement per move (list)
    Function closeList;       // runs when a card is chosen: unsubscribes the list from the window list (list)
    Function findDescendant;  // recursive search for an element of kSearchedType (root)
    Function vectorRemoveAt;  // the card vector's RemoveAt(vector, index)
    Function vectorInsertAt;  // InsertAt(vector, index, card)
    Function vectorNotify;    // and its change notifier(vector, change, index)
    uint32_t listCards;       // card list member: the card vector
    uint32_t listClosed;      // card list member: the byte closeList sets
    uint32_t vectorStorage;   // card vector member: its begin/end pointer pair
    uint32_t vectorObserved;  // card vector member: notifications are raised only while this byte is set
};

inline constexpr uint32_t kMagic = 0x42415441;  // "ATAB"
inline constexpr uint32_t kVersion = 1;

// Passed to the hook DLL's Start as its parameter.
struct Internals {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t size = sizeof(Internals);
    Twinui twinui{};
    Switcher switcher{};
};

}  // namespace internals
