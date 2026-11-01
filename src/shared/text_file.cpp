#include "text_file.h"

#include <windows.h>

#include <fstream>
#include <iterator>

std::wstring ReadTextFile(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    std::string utf8((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (utf8.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring text(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), text.data(), length);
    return text;
}

bool WriteTextFile(const std::wstring& path, const std::wstring& text) {
    int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    return static_cast<bool>(file.write(utf8.data(), static_cast<std::streamsize>(utf8.size())));
}
