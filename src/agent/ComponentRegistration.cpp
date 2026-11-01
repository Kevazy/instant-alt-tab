#include "ComponentRegistration.h"

#include <windows.h>

namespace {

constexpr wchar_t kActivatableClasses[] = L"SOFTWARE\\Microsoft\\WindowsRuntime\\ActivatableClassId";

bool NamesFile(const wchar_t* path, const wchar_t* fileName) {
    const wchar_t* slash = wcsrchr(path, L'\\');
    return _wcsicmp(slash ? slash + 1 : path, fileName) == 0;
}

}  // namespace

std::wstring RegisteredComponentPath(const wchar_t* fileName) {
    HKEY classes;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kActivatableClasses, 0, KEY_READ, &classes) != ERROR_SUCCESS) return L"";
    std::wstring found;
    bool ambiguous = false;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];  // the longest a key name can be, plus its terminator
        DWORD length = ARRAYSIZE(name);
        if (RegEnumKeyExW(classes, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t path[MAX_PATH];
        DWORD size = sizeof(path);
        // REG_EXPAND_SZ paths come back expanded.
        if (RegGetValueW(classes, name, L"DllPath", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS ||
            !NamesFile(path, fileName)) {
            continue;
        }
        if (found.empty()) {
            found = path;
        } else if (_wcsicmp(found.c_str(), path) != 0) {
            ambiguous = true;
        }
    }
    RegCloseKey(classes);
    return ambiguous ? L"" : found;
}
