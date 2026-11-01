#include "Alert.h"

#include "../shared/SharedNames.h"
#include "../shared/TextFile.h"

#include <windows.h>

#include <algorithm>
#include <sstream>
#include <thread>
#include <vector>

namespace Alert {

constexpr size_t kMaxRemembered = 20;  // the oldest keys are forgotten first

void Show(const std::wstring& text) {
    // On top of other windows, but not forced into the foreground: Windows
    // normally doesn't give a background process the keyboard focus, so typing
    // elsewhere isn't interrupted, and the taskbar button flashes instead.
    std::thread([text] {
        MessageBoxW(nullptr, text.c_str(), L"Instant Alt+Tab", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }).detach();
}

void ShowOnce(const std::wstring& directory, const std::wstring& key, const std::wstring& text) {
    std::wstring path = directory + SharedNames::kNotifiedFile;
    std::vector<std::wstring> shown;
    std::wstringstream lines(ReadTextFile(path));
    for (std::wstring line; std::getline(lines, line);) shown.push_back(line);
    if (std::find(shown.begin(), shown.end(), key) != shown.end()) return;
    shown.push_back(key);
    if (shown.size() > kMaxRemembered) shown.erase(shown.begin(), shown.end() - kMaxRemembered);
    std::wstring content;
    for (const std::wstring& line : shown) content += line + L"\n";
    WriteTextFile(path, content);
    Show(text);
}

}  // namespace Alert
