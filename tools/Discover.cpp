// discover.exe: runs the agent's discovery on any copies of twinui.pcshell.dll
// and the switcher DLL and prints what it finds, one value per line. Used to
// check the rules against other Windows builds (old copies are kept in
// C:\Windows\WinSxS) without touching Explorer.
//
// usage: discover [--twinui PATH] [--switcher PATH] [--cache DIR]

#include "../src/discovery/Discovery.h"

#include <objbase.h>

#include <cstdio>
#include <string>

namespace {

std::wstring SystemPath(const wchar_t* relative) {
    wchar_t windows[MAX_PATH];
    GetWindowsDirectoryW(windows, MAX_PATH);
    return std::wstring(windows) + relative;
}

void Print(const wchar_t* name, uint32_t value) { wprintf(L"%-28s 0x%x\n", name, value); }

void Print(const wchar_t* name, const AltTab::FunctionLocation& function) {
    wprintf(L"%-28s 0x%x  ", name, function.rva);
    for (uint8_t byte : function.entry) wprintf(L"%02X", byte);
    wprintf(L"\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring twinui = SystemPath(L"\\System32\\twinui.pcshell.dll");
    std::wstring switcher = SystemPath(L"\\ShellComponents\\WindowsInternal.ComposableShell.Experiences.Switcher.dll");
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring cache = std::wstring(temp) + L"instant-alt-tab-symbols\\";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::wstring option = argv[i];
        if (option == L"--twinui")
            twinui = argv[i + 1];
        else if (option == L"--switcher")
            switcher = argv[i + 1];
        else if (option == L"--cache")
            cache = std::wstring(argv[i + 1]) + L"\\";
    }
    CreateDirectoryW(cache.c_str(), nullptr);

    ULONGLONG started = GetTickCount64();
    Discovery::Result result = Discovery::Discover(twinui, switcher, cache);
    if (!result.internals) {
        fwprintf(stderr, L"not found%s: %s\n", result.retryLater ? L" (retry later)" : L"", result.error.c_str());
        return 1;
    }
    const AltTab::TwinuiInternals& t = result.internals->twinui;
    const AltTab::SwitcherInternals& s = result.internals->switcher;
    Print(L"twinui.timeDateStamp", t.module.timeDateStamp);
    Print(L"twinui.sizeOfImage", t.module.sizeOfImage);
    Print(L"createControl", t.createControl);
    Print(L"show", t.show);
    Print(L"dismiss", t.dismiss);
    Print(L"display", t.display);
    Print(L"setHostContent", t.setHostContent);
    Print(L"registerRoot", t.registerRoot);
    Print(L"getItemForView", t.getItemForView);
    Print(L"registerRootReturnInShow", t.registerRootReturnInShow);
    Print(L"hostContentGetter", t.hostContentGetter);
    Print(L"hostContentSetter", t.hostContentSetter);
    Print(L"hostContentGetterSlot", t.hostContentGetterSlot);
    Print(L"hostContentSetterSlot", t.hostContentSetterSlot);
    Print(L"hostDataSource", t.hostDataSource);
    Print(L"hostControl", t.hostControl);
    Print(L"hostWindow", t.hostWindow);
    Print(L"navigateToItemSlot", t.navigateToItemSlot);
    Print(L"navigateSlot", t.navigateSlot);
    wchar_t id[40];
    StringFromGUID2(t.switchItem, id, 40);
    wprintf(L"%-28s %s\n", L"switchItem", id);
    StringFromGUID2(t.switchItemDataSource, id, 40);
    wprintf(L"%-28s %s\n", L"switchItemDataSource", id);
    Print(L"switcher.timeDateStamp", s.module.timeDateStamp);
    Print(L"switcher.sizeOfImage", s.module.sizeOfImage);
    Print(L"syncList", s.syncList);
    Print(L"sortList", s.sortList);
    Print(L"closeList", s.closeList);
    Print(L"findDescendant", s.findDescendant);
    Print(L"vectorRemoveAt", s.vectorRemoveAt);
    Print(L"vectorInsertAt", s.vectorInsertAt);
    Print(L"vectorNotify", s.vectorNotify);
    Print(L"listCards", s.listCards);
    Print(L"listClosed", s.listClosed);
    Print(L"vectorStorage", s.vectorStorage);
    Print(L"vectorObserved", s.vectorObserved);
    fwprintf(stderr, L"found in %llu ms\n", GetTickCount64() - started);
    return 0;
}
