#pragma once

// Everything this project knows about Windows internals, for the one Windows
// build it was reverse-engineered against. Nothing is hooked unless both
// modules match the build below and every function's entry bytes match, so a
// Windows update turns the hooks off instead of breaking Explorer.
//
// tools/find_offsets.py derives these values for another build, and
// docs/internals.md explains how each one is used.

#include <windows.h>

#include <array>
#include <cstddef>

namespace pins {

struct Module {
    const wchar_t* name;
    DWORD timeDateStamp;
    DWORD sizeOfImage;
};

struct Function {
    DWORD rva;
    std::array<BYTE, 16> entry;  // first bytes of the function, compared before hooking
};

// --- twinui.pcshell.dll 10.0.26100.8524: hosts the Alt+Tab view (public symbols) ---

inline constexpr Module kTwinui{L"twinui.pcshell.dll", 0x1c8ab037, 0x999000};

// XamlAltTabViewHost::CreateAltTabControl(host) -> IUnknown* control
inline constexpr Function kCreateControl{
    0x3f2bc, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55}};
// XamlAltTabViewHost::Show(host, display, flags, view) -> HRESULT
inline constexpr Function kShow{
    0xd7ec0, {0x48, 0x8B, 0xC4, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81}};
// XamlAltTabViewHost::v_DismissView(host)
inline constexpr Function kDismiss{
    0xda320, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0xB9}};
// XamlAltTabViewHost::DisplayAltTab(host)
inline constexpr Function kDisplay{
    0xd90d8, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41}};
// Sets the host window's XAML content: (IUnknown** window, IUnknown** content).
// The linker folded several identical wrappers into this one function.
inline constexpr Function kSetHostContent{
    0xd79f8, {0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x09, 0x0F, 0x57, 0xC0, 0x48, 0x8B, 0x12, 0x83, 0x64, 0x24}};
// Registers a XAML root: (registrar, root) -> registration. Folded with GetAt(index),
// so only the call from Show (identified by its return address) passes a root.
inline constexpr Function kRegisterRoot{
    0xdbc84, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0x64, 0x24, 0x30, 0x00, 0x48, 0x8B, 0x01, 0x4C}};
inline constexpr DWORD kRegisterRootReturnInShow = 0xd83ef;
inline constexpr std::array<BYTE, 5> kRegisterRootCallInShow{0xE8, 0x95, 0x38, 0x00, 0x00};
// CachedDataSource::GetItemForView(source, view, ...) -> the foreground window's item, or null
inline constexpr Function kGetItemForView{
    0xd8dc0, {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x85, 0xD2, 0x74}};

// IXamlExplorerHostWindow: the get/put content methods, and their vtable slots.
inline constexpr DWORD kHostContentGetter = 0x29ca80, kHostContentSetter = 0xa3ac0;
inline constexpr size_t kHostContentGetterSlot = 8, kHostContentSetterSlot = 9;

// XamlAltTabViewHost members.
inline constexpr size_t kHostDataSource = 0x98, kHostControl = 0xa0, kHostWindow = 0xa8;

// Show's flags: the menu was opened backwards (Alt+Shift+Tab).
inline constexpr int kShowBackward = 0x2;

// The AltTab control's interface at kHostControl: vtable slots of NavigateToItem(item)
// and Navigate(backward), implemented in the switcher (kNavigateToItem, kNavigate).
inline constexpr size_t kNavigateToItemSlot = 19, kNavigateSlot = 20;

// --- WindowsInternal.ComposableShell.Experiences.Switcher.dll 10.0.26100.8972 ---
// The Alt+Tab UI. No public symbols: found by code structure.

inline constexpr Module kSwitcher{L"WindowsInternal.ComposableShell.Experiences.Switcher.dll", 0xb1ab7d2e, 0x31c000};

// SwitchItemListViewModel: re-syncs its cards with the data model (list).
inline constexpr Function kSyncList{
    0x74ca0, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55}};
// SwitchItemListViewModel: sorts its cards in place, one SetAt per move (list).
inline constexpr Function kSortList{
    0x1a930, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56}};
// SwitchItemListViewModel: runs when a card is chosen. Sets kListClosed and
// unregisters the list's change handler from its data source (list).
inline constexpr Function kCloseList{
    0x99018, {0xC6, 0x81, 0x2A, 0x01, 0x00, 0x00, 0x01, 0xE9, 0x04, 0x4F, 0xFD, 0xFF, 0xCC, 0xCC, 0xCC, 0xCC}};
// Recursive search for the first descendant implementing kSearchedType (root) -> element or null.
inline constexpr Function kFindDescendant{
    0x142804, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x4C}};
// AltTab control: NavigateToItem(control, item) and Navigate(control, backward) -> HRESULT.
// Show positions the selection through them, and so does every Tab while the menu is open.
inline constexpr Function kNavigateToItem{
    0xa6790, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48}};
inline constexpr Function kNavigate{
    0xa6740, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x40}};
// The card vector (Platform::Collections::Vector): RemoveAt(vector, index),
// InsertAt(vector, index, card) and its change notifier(vector, change, index).
inline constexpr Function kVectorRemoveAt{
    0x183ed0, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC}};
inline constexpr Function kVectorInsertAt{
    0x1831dc, {0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0x05, 0x15, 0xAF, 0x13, 0x00, 0x48}};
inline constexpr Function kVectorNotify{
    0x68608, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x41}};

// SwitchItemListViewModel members: the card vector, and the byte set by kCloseList.
inline constexpr size_t kListCards = 0x110, kListClosed = 0x12a;
// Card vector members: storage (a begin/end pointer pair) and the "observed" flag;
// the notifier raises VectorChanged only while the flag is set.
inline constexpr size_t kVectorStorage = 0x60, kVectorObserved = 0x70;
// Card view model member: the card's ISwitchItem.
inline constexpr size_t kCardSwitchItem = 0xd8;

// ISwitchItem {76593595-88f6-40c9-bd90-6f1a79aec5b8}: the shell's item behind each card.
inline constexpr GUID kISwitchItem{0x76593595, 0x88f6, 0x40c9, {0xbd, 0x90, 0x6f, 0x1a, 0x79, 0xae, 0xc5, 0xb8}};
// The element type kFindDescendant looks for; the Alt+Tab menu never contains one.
inline constexpr GUID kSearchedType{0xae506dd8, 0xf75c, 0x3cce, {0xbe, 0x60, 0xd9, 0x82, 0x7a, 0xb2, 0x44, 0x4e}};

}  // namespace pins
